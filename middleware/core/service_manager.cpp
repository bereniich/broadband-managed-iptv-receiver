#include "service_manager.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace al {

namespace {

constexpr const char* kModule = "AL-SVC";

using nlohmann::json;

json parse(const std::string& body) { return json::parse(body, nullptr, false); }

// Code of an error body {"error": {"code": …, "message": …}} (Section 3.2), or "".
std::string error_code(const json& body) {
    if (!body.is_object()) return "";
    auto error = body.find("error");
    if (error == body.end() || !error->is_object()) return "";
    auto code = error->find("code");
    return code != error->end() && code->is_string() ? code->get<std::string>() : "";
}

bool non_empty_string(const json& object, const char* field) {
    auto it = object.find(field);
    return it != object.end() && it->is_string() && !it->get<std::string>().empty();
}

bool integer(const json& object, const char* field) {
    auto it = object.find(field);
    return it != object.end() && it->is_number_integer();
}

std::string transport_text(const pal::HttpResponse& r) {
    if (!r.transport_ok) return r.error.empty() ? "transport failure" : r.error;
    return "status " + std::to_string(r.status);
}

// Reason why an entry of GET /services is invalid (REQ-02), or "".
std::string invalid_entry(const json& e, const std::set<int>& numbers, const std::set<std::string>& ids) {
    if (!e.is_object()) return "not an object";
    if (!non_empty_string(e, "service_id")) return "service_id missing or empty";
    if (!integer(e, "number")) return "number missing or not an integer";
    const std::int64_t number = e["number"].get<std::int64_t>();
    if (number < 1 || number > 999) return "number out of range";
    if (!non_empty_string(e, "name")) return "name missing or empty";
    auto p = e.find("protected");
    if (p == e.end() || !p->is_boolean()) return "protected missing or not a boolean";
    if (numbers.count(static_cast<int>(number)) != 0) return "duplicate number";
    if (ids.count(e["service_id"].get<std::string>()) != 0) return "duplicate service_id";
    return "";
}

// Reason why an EPG event is invalid (REQ-32, Section 3.2.3), or "".
std::string invalid_event(const json& e, std::optional<std::int64_t> previous_end) {
    if (!e.is_object()) return "not an object";
    if (!non_empty_string(e, "event_id")) return "event_id missing";
    if (!integer(e, "start")) return "start missing or not an integer";
    if (!integer(e, "duration") || e["duration"].get<std::int64_t>() <= 0) return "duration missing or invalid";
    auto title = e.find("title");
    if (title == e.end() || !title->is_string()) return "title missing";
    auto description = e.find("description");
    if (description == e.end() || !description->is_string()) return "description missing";
    auto in_archive = e.find("in_archive");
    if (in_archive == e.end() || !in_archive->is_boolean()) return "in_archive missing";
    if (previous_end && e["start"].get<std::int64_t>() < *previous_end) return "not sorted or overlaps the previous event";
    return "";
}

}  // namespace

const char* to_code(MwError error) {
    switch (error) {
        case MwError::None: return "";
        case MwError::MwUnreachable: return "MW_UNREACHABLE";
        case MwError::ServiceNotFound: return "SERVICE_NOT_FOUND";
        case MwError::InvalidRange: return "INVALID_RANGE";
    }
    return "";
}

ServiceManager::ServiceManager(pal::IHttpClient& http, pal::IClock& clock, pal::ILogger& logger, TaskQueue& queue,
                               Config config)
    : http_(http), clock_(clock), logger_(logger), queue_(queue), config_(std::move(config)) {}

ServiceManager::~ServiceManager() {
    if (retry_timer_) clock_.cancel_timer(*retry_timer_);
    if (services_request_) http_.cancel(*services_request_);
    for (const auto& entry : pending_) {
        if (entry.second.http) http_.cancel(*entry.second.http);
    }
}

void ServiceManager::load_services() {
    if (loaded_ || loading_) return;
    loading_ = true;
    TaskQueue& queue = queue_;
    services_request_ = http_.get(config_.mw_base_url + "/services", config_.timeout_ms,
                                  [this, &queue](const pal::HttpResponse& r) {
                                      queue.post(TaskKind::Control, [this, r] { on_services(r); });
                                  });
}

const ServiceEntry* ServiceManager::find(const std::string& service_id) const {
    auto it = index_.find(service_id);
    return it == index_.end() ? nullptr : &services_[it->second];
}

ServiceManager::Token ServiceManager::get_service_info(const std::string& service_id, InfoDone done) {
    const Token token = next_token_++;
    Pending& p = pending_[token];
    p.info_done = std::move(done);
    p.service_id = service_id;
    if (info_cache_.count(service_id) != 0) {
        // From the cache, but still as a task of its own, so that AL-SES always gets the result later.
        queue_.post(TaskKind::Control, [this, token] {
            auto it = pending_.find(token);
            if (it == pending_.end()) return;   // cancelled
            InfoDone callback = std::move(it->second.info_done);
            const std::string id = it->second.service_id;
            pending_.erase(it);
            if (callback) callback(MwError::None, &info_cache_.at(id));
        });
        return token;
    }
    TaskQueue& queue = queue_;
    p.http = http_.get(config_.mw_base_url + "/services/" + service_id, config_.timeout_ms,
                       [this, &queue, token](const pal::HttpResponse& r) {
                           queue.post(TaskKind::Control, [this, token, r] { on_service_info(token, r); });
                       });
    return token;
}

ServiceManager::Token ServiceManager::get_epg(std::int64_t from, std::int64_t to, EpgDone done) {
    const Token token = next_token_++;
    Pending& p = pending_[token];
    p.epg_done = std::move(done);
    TaskQueue& queue = queue_;
    p.http = http_.get(config_.mw_base_url + "/epg?from=" + std::to_string(from) + "&to=" + std::to_string(to),
                       config_.timeout_ms, [this, &queue, token](const pal::HttpResponse& r) {
                           queue.post(TaskKind::Control, [this, token, r] { on_epg(token, r); });
                       });
    return token;
}

void ServiceManager::cancel(Token token) {
    auto it = pending_.find(token);
    if (it == pending_.end()) return;
    if (it->second.http) http_.cancel(*it->second.http);
    pending_.erase(it);
}

void ServiceManager::on_services(const pal::HttpResponse& r) {
    loading_ = false;
    services_request_.reset();
    if (loaded_) return;
    const json body = r.transport_ok ? parse(r.body) : json(json::value_t::discarded);
    if (!r.transport_ok || r.status != 200 || !body.is_object() || !body.contains("services") ||
        !body["services"].is_array()) {
        set_reachable(false);   // REQ-01: any failure of the service list
        retry_later(transport_text(r) + (r.transport_ok && r.status == 200 ? ", invalid body" : ""));
        return;
    }

    std::vector<ServiceEntry> list;
    std::set<int> numbers;
    std::set<std::string> ids;
    const json& entries = body["services"];
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string reason = invalid_entry(entries[i], numbers, ids);
        if (!reason.empty()) {
            log(pal::LogLevel::Warn, "discarded service entry " + std::to_string(i) + ": " + reason);
            continue;
        }
        ServiceEntry e;
        e.service_id = entries[i]["service_id"].get<std::string>();
        e.number = entries[i]["number"].get<int>();
        e.name = entries[i]["name"].get<std::string>();
        e.is_protected = entries[i]["protected"].get<bool>();
        numbers.insert(e.number);
        ids.insert(e.service_id);
        list.push_back(std::move(e));
    }
    std::sort(list.begin(), list.end(), [](const ServiceEntry& a, const ServiceEntry& b) { return a.number < b.number; });
    services_ = std::move(list);
    index_.clear();
    for (std::size_t i = 0; i < services_.size(); ++i) index_[services_[i].service_id] = i;
    loaded_ = true;
    log(pal::LogLevel::Info, "services loaded count=" + std::to_string(services_.size()));
    set_reachable(true);
}

void ServiceManager::on_service_info(Token token, const pal::HttpResponse& r) {
    auto it = pending_.find(token);
    if (it == pending_.end()) return;   // cancelled
    InfoDone done = std::move(it->second.info_done);
    const std::string id = it->second.service_id;
    pending_.erase(it);

    const json body = r.transport_ok ? parse(r.body) : json(json::value_t::discarded);
    if (r.transport_ok && r.status == 200 && body.is_object() && non_empty_string(body, "stream_control_url") &&
        integer(body, "archive_window_s") && body["archive_window_s"].get<std::int64_t>() > 0) {
        note_reachability(r, true);
        ServiceInfo info;
        info.stream_control_url = body["stream_control_url"].get<std::string>();
        info.archive_window_s = body["archive_window_s"].get<int>();
        const ServiceInfo& cached = info_cache_[id] = info;
        if (done) done(MwError::None, &cached);
        return;
    }
    const std::string code = error_code(body);
    note_reachability(r, r.transport_ok && r.status != 200 && !code.empty());
    if (r.transport_ok && r.status == 404 && code == "SERVICE_NOT_FOUND") {
        log(pal::LogLevel::Warn, "GET /services/" + id + ": SERVICE_NOT_FOUND");
        if (done) done(MwError::ServiceNotFound, nullptr);
        return;
    }
    log(pal::LogLevel::Warn, "GET /services/" + id + " failed: " + transport_text(r));
    if (done) done(MwError::MwUnreachable, nullptr);
}

void ServiceManager::on_epg(Token token, const pal::HttpResponse& r) {
    auto it = pending_.find(token);
    if (it == pending_.end()) return;   // cancelled
    EpgDone done = std::move(it->second.epg_done);
    pending_.erase(it);

    const json body = r.transport_ok ? parse(r.body) : json(json::value_t::discarded);
    const bool valid = r.transport_ok && r.status == 200 && body.is_object() && integer(body, "server_time") &&
                       body.contains("services") && body["services"].is_array();
    if (!valid) {
        const std::string code = error_code(body);
        note_reachability(r, r.transport_ok && r.status != 200 && !code.empty());
        if (r.transport_ok && r.status == 400 && code == "INVALID_RANGE") {
            log(pal::LogLevel::Warn, "GET /epg: INVALID_RANGE");
            if (done) done(MwError::InvalidRange, nullptr);
            return;
        }
        log(pal::LogLevel::Warn, "GET /epg failed: " + transport_text(r) +
                                     (r.transport_ok && r.status == 200 ? ", invalid body" : ""));
        if (done) done(MwError::MwUnreachable, nullptr);
        return;
    }
    note_reachability(r, true);

    json result = {{"server_time", body["server_time"]}, {"services", json::array()}};
    for (const json& service : body["services"]) {
        if (!service.is_object() || !non_empty_string(service, "service_id") || !service.contains("events") ||
            !service["events"].is_array()) {
            log(pal::LogLevel::Warn, "discarded EPG service entry: service_id or events missing");
            continue;
        }
        json events = json::array();
        std::optional<std::int64_t> previous_end;
        for (const json& event : service["events"]) {
            const std::string reason = invalid_event(event, previous_end);
            if (!reason.empty()) {
                const std::string id = event.is_object() && event.contains("event_id") && event["event_id"].is_string()
                                           ? event["event_id"].get<std::string>()
                                           : "(no event_id)";
                log(pal::LogLevel::Warn, "discarded EPG event " + id + ": " + reason);
                continue;
            }
            previous_end = event["start"].get<std::int64_t>() + event["duration"].get<std::int64_t>();
            events.push_back(event);
        }
        result["services"].push_back({{"service_id", service["service_id"]}, {"events", std::move(events)}});
    }
    if (done) done(MwError::None, &result);
}

void ServiceManager::retry_later(const std::string& why) {
    log(pal::LogLevel::Warn, "GET /services failed: " + why + ", retry in " + std::to_string(config_.retry_s) + " s");
    TaskQueue& queue = queue_;
    retry_timer_ = clock_.start_timer(config_.retry_s * 1000, [this, &queue] {
        queue.post(TaskKind::Control, [this] {
            retry_timer_.reset();
            load_services();
        });
    });
}

void ServiceManager::note_reachability(const pal::HttpResponse& r, bool valid_body) {
    set_reachable(r.transport_ok && valid_body);
}

void ServiceManager::set_reachable(bool reachable) {
    if (reachable_ && *reachable_ == reachable) return;
    reachable_ = reachable;
    log(pal::LogLevel::Info, std::string("mw reachable=") + (reachable ? "true" : "false"));
    if (status_handler_) status_handler_(reachable);
}

void ServiceManager::log(pal::LogLevel level, const std::string& message) {
    logger_.log(level, kModule, message);
}

}  // namespace al
