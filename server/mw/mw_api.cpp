#include "mw_api.hpp"

#include <limits>
#include <optional>

namespace srv {

namespace {

constexpr const char* kModule = "SRV-MW";
constexpr const char* kBase = "/api/v1";

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

}  // namespace

std::vector<EpgEvent> epg_events(const Service& service, int64_t from, int64_t to) {
    std::vector<EpgEvent> events;
    const int64_t cycle = service.cycle_duration();
    if (cycle <= 0 || from >= to) return events;
    // 128-bit arithmetic, so that times near the int64_t limits cannot overflow; an event whose
    // start does not fit int64_t is left out.
    const __int128 min_start = std::numeric_limits<int64_t>::min();
    const int64_t k_first = floor_div(from, cycle) - 1;
    const int64_t k_last = floor_div(to, cycle);
    for (int64_t k = k_first;; ++k) {
        __int128 offset = 0;
        for (const Program& p : service.programs) {
            const __int128 start = static_cast<__int128>(k) * cycle + offset;
            offset += p.duration;
            if (start < to && start + p.duration > from && start >= min_start) {
                events.push_back({static_cast<int64_t>(start), p.duration, &p});
            }
        }
        if (k == k_last) break;
    }
    return events;
}

MwApi::MwApi(ServiceCatalog catalog, std::string stream_control_url, WallClock clock, Logger* logger)
    : catalog_(std::move(catalog)),
      stream_control_url_(std::move(stream_control_url)),
      clock_(std::move(clock)),
      logger_(logger) {}

HttpResponse MwApi::handle(const HttpRequest& request) const {
    HttpResponse response = route(request);
    if (logger_ != nullptr) {
        std::string target = request.path;
        if (!request.query.empty()) target += "?" + request.query;
        if (target.size() > 200) target = target.substr(0, 200) + "...";
        std::string line = request.method.substr(0, 16) + " " + target + " -> " + std::to_string(response.status);
        if (response.error_code.empty()) {
            logger_->info(kModule, line);
        } else {
            logger_->warn(kModule, line + " " + response.error_code + ": " + response.error_message);
        }
    }
    return response;
}

HttpResponse MwApi::route(const HttpRequest& request) const {
    const std::string& path = request.path;
    const std::string services_path = std::string(kBase) + "/services";
    const std::string epg_path = std::string(kBase) + "/epg";
    const bool is_services = path == services_path;
    const bool is_service = path.compare(0, services_path.size() + 1, services_path + "/") == 0;
    const bool is_epg = path == epg_path;

    // Error codes for requests outside IF-EXT-03 (ER-18): Section 3.2 allows 400 and 404 with
    // an error object; INVALID_REQUEST is the code of Section 3.10 for a malformed request.
    if (!is_services && !is_service && !is_epg) {
        return error_response(404, "INVALID_REQUEST", "unknown path");
    }
    if (request.method != "GET") {
        return error_response(400, "INVALID_REQUEST", "method not allowed, only GET");
    }
    if (is_services) return services();
    if (is_service) return service(path.substr(services_path.size() + 1));
    return epg(request.query);
}

HttpResponse MwApi::services() const {
    nlohmann::ordered_json list = nlohmann::ordered_json::array();
    for (const Service& s : catalog_.services) {
        nlohmann::ordered_json entry;
        entry["service_id"] = s.service_id;
        entry["number"] = s.number;
        entry["name"] = s.name;
        entry["protected"] = s.is_protected;
        list.push_back(std::move(entry));
    }
    nlohmann::ordered_json body;
    body["services"] = std::move(list);
    return json_response(200, body);
}

HttpResponse MwApi::service(const std::string& service_id) const {
    const Service* s = catalog_.find(service_id);
    if (s == nullptr) return error_response(404, "SERVICE_NOT_FOUND", "unknown service_id");
    nlohmann::ordered_json body;
    body["service_id"] = s->service_id;
    body["number"] = s->number;
    body["name"] = s->name;
    body["protected"] = s->is_protected;
    body["description"] = s->description;
    body["stream_control_url"] = stream_control_url_;
    body["archive_window_s"] = catalog_.archive_window_s;
    return json_response(200, body);
}

HttpResponse MwApi::epg(const std::string& query) const {
    const auto from_text = query_var(query, "from");
    const auto to_text = query_var(query, "to");
    if (!from_text || !to_text) return error_response(400, "INVALID_RANGE", "from and to are required");
    const auto from = parse_int64(*from_text);
    const auto to = parse_int64(*to_text);
    if (!from || !to) return error_response(400, "INVALID_RANGE", "from and to must be integers");
    if (*from >= *to) return error_response(400, "INVALID_RANGE", "from must be less than to");
    // from < to, so the unsigned difference is exact for every pair of int64_t values.
    if (static_cast<uint64_t>(*to) - static_cast<uint64_t>(*from) > static_cast<uint64_t>(kMaxEpgRange)) {
        return error_response(400, "INVALID_RANGE", "to - from must not exceed 86400");
    }

    std::vector<const Service*> selected;
    if (const auto id = query_var(query, "service_id")) {
        const Service* s = catalog_.find(*id);
        if (s == nullptr) return error_response(404, "SERVICE_NOT_FOUND", "unknown service_id");
        selected.push_back(s);
    } else {
        for (const Service& s : catalog_.services) selected.push_back(&s);
    }

    // Read once, so that every event of the response uses the same server_time (Section 4.7.7).
    const int64_t server_time = unix_seconds(clock_());
    const int64_t archive_begin = server_time - catalog_.archive_window_s;

    nlohmann::ordered_json services = nlohmann::ordered_json::array();
    for (const Service* s : selected) {
        nlohmann::ordered_json events = nlohmann::ordered_json::array();
        for (const EpgEvent& e : epg_events(*s, *from, *to)) {
            nlohmann::ordered_json event;
            event["event_id"] = s->service_id + "-" + std::to_string(e.start);
            event["start"] = e.start;
            event["duration"] = e.duration;
            event["title"] = e.program->title;
            event["description"] = e.program->description;
            event["in_archive"] = archive_begin <= e.start && e.start <= server_time;
            events.push_back(std::move(event));
        }
        nlohmann::ordered_json entry;
        entry["service_id"] = s->service_id;
        entry["events"] = std::move(events);
        services.push_back(std::move(entry));
    }
    nlohmann::ordered_json body;
    body["server_time"] = server_time;
    body["from"] = *from;
    body["to"] = *to;
    body["services"] = std::move(services);
    return json_response(200, body);
}

}  // namespace srv
