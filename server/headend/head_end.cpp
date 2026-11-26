#include "head_end.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "config.hpp"

namespace srv {

namespace {

constexpr const char* kModule = "SRV-HEC";

// String field, or nullptr if missing, not a string or empty.
const std::string* string_field(const nlohmann::json& body, const char* key) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) return nullptr;
    return &it->get_ref<const std::string&>();
}

std::optional<int64_t> int_field(const nlohmann::json& body, const char* key, int64_t min, int64_t max) {
    const auto it = body.find(key);
    if (it == body.end()) return std::nullopt;
    const auto value = json_int64(*it);
    if (!value || *value < min || *value > max) return std::nullopt;
    return value;
}

HttpResponse invalid(const std::string& message) { return error_response(400, "INVALID_REQUEST", message); }

}  // namespace

HeadEnd::HeadEnd(ServiceCatalog catalog, std::map<std::string, std::shared_ptr<const Archive>> archives,
                 HeadEndOptions options, Logger* logger)
    : catalog_(std::move(catalog)), archives_(std::move(archives)), options_(std::move(options)), logger_(logger) {
    for (const Service& s : catalog_.services) {
        if (!archives_.count(s.service_id)) throw std::runtime_error("no archive for " + s.service_id);
    }
    rng_.seed(options_.seed != 0 ? options_.seed : (uint64_t{std::random_device{}()} << 32) ^ std::random_device{}());
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (udp_fd_ < 0) throw std::runtime_error(std::string("cannot create the UDP socket: ") + std::strerror(errno));
    if (options_.start_watchdog) {
        watchdog_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(watchdog_mutex_);
            while (!watchdog_cv_.wait_for(lock, options_.watchdog_period, [this] { return watchdog_stop_; })) {
                lock.unlock();
                watchdog_tick();
                lock.lock();
            }
        });
    }
}

HeadEnd::~HeadEnd() {
    shutdown();
    if (udp_fd_ >= 0) ::close(udp_fd_);
}

void HeadEnd::shutdown() {
    {
        std::lock_guard<std::mutex> lock(watchdog_mutex_);
        watchdog_stop_ = true;
    }
    watchdog_cv_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    while (!sessions_.empty()) {
        const std::string id = sessions_.begin()->first;
        remove_locked(id);
        if (logger_) logger_->info(kModule, "stop session_id=" + id + " reason=shutdown");
    }
}

std::vector<std::string> HeadEnd::session_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& entry : sessions_) ids.push_back(entry.first);
    return ids;
}

void HeadEnd::remove_locked(const std::string& session_id) {
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;
    it->second->sender->stop();  // returns after the sender thread has ended
    by_endpoint_.erase(it->second->endpoint);
    sessions_.erase(it);
}

void HeadEnd::watchdog_tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = options_.session_clock();
    std::vector<std::pair<std::string, double>> expired;
    for (const auto& [id, session] : sessions_) {
        if (now - session->last_seen >= options_.session_expiry) {
            expired.emplace_back(id, std::chrono::duration<double>(now - session->last_seen).count());
        }
    }
    for (const auto& [id, idle] : expired) {
        remove_locked(id);
        if (logger_) {
            char text[48];
            std::snprintf(text, sizeof(text), "%.1f", idle);
            logger_->warn(kModule, "session expired session_id=" + id + " (no start or keepalive for " + text + " s)");
        }
    }
}

HttpResponse HeadEnd::handle(const HttpRequest& request) {
    HttpResponse response = route(request);
    if (logger_ && !response.error_code.empty()) {
        std::string target = request.path;
        if (target.size() > 200) target = target.substr(0, 200) + "...";
        logger_->warn(kModule, request.method.substr(0, 16) + " " + target + " -> " + std::to_string(response.status) +
                                   " " + response.error_code + ": " + response.error_message);
    }
    return response;
}

HttpResponse HeadEnd::route(const HttpRequest& request) {
    const std::string base = kBasePath;
    const std::string& path = request.path;
    const bool is_start = path == base + "/start";
    const bool is_stop = path == base + "/stop";
    const bool is_keepalive = path == base + "/keepalive";
    if (!is_start && !is_stop && !is_keepalive) return error_response(404, "INVALID_REQUEST", "unknown path");
    if (request.method != "POST") return invalid("method not allowed, only POST");
    const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) return invalid("body is not valid JSON");
    if (!body.is_object()) return invalid("body is not a JSON object");
    if (is_start) return start(body);
    if (is_keepalive) return keepalive(body);
    return stop(body);
}

HttpResponse HeadEnd::start(const nlohmann::json& body) {
    // 1. Validation of the fields (Section 3.3.1).
    const std::string* service_id = string_field(body, "service_id");
    if (!service_id) return invalid("service_id missing or not a non-empty string");
    const auto start_it = body.find("start");
    if (start_it == body.end()) return invalid("start missing");
    const bool live = start_it->is_string() && start_it->get_ref<const std::string&>() == "live";
    const std::optional<int64_t> requested = live ? std::nullopt : json_int64(*start_it);
    if (!live && !requested) return invalid("start must be \"live\" or an integer UNIX time");
    const std::string* host = string_field(body, "client_host");
    in_addr address{};
    if (!host || inet_pton(AF_INET, host->c_str(), &address) != 1) {
        return invalid("client_host missing or not an IPv4 address");
    }
    const auto port = int_field(body, "client_port", 1, 65535);
    if (!port) return invalid("client_port missing or not an integer 1 - 65535");
    const auto ssrc = int_field(body, "ssrc", 0, 4294967295LL);
    if (!ssrc) return invalid("ssrc missing or not an unsigned 32-bit integer");

    // 2. Service known.
    if (catalog_.find(*service_id) == nullptr) return error_response(404, "SERVICE_NOT_FOUND", "unknown service_id");

    // 3. Archive window (Section 3.1); "live" starts at the current server time.
    const int64_t now = unix_seconds(options_.wall_clock());
    const int64_t t = live ? now : *requested;
    if (!live) {
        switch (check_archive_window(t, now, catalog_.archive_window_s)) {
            case WindowCheck::OutOfArchive:
                return error_response(400, "OUT_OF_ARCHIVE", "start is before the archive window");
            case WindowCheck::FutureTime:
                return error_response(400, "FUTURE_TIME", "start is not in the past, use \"live\"");
            case WindowCheck::Ok:
                break;
        }
    }

    // 4. Under the lock: stop the session of the same endpoint, position, new session (Section 4.7.6).
    const std::string endpoint = *host + ":" + std::to_string(*port);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto old = by_endpoint_.find(endpoint);
    if (old != by_endpoint_.end()) {
        const std::string old_id = old->second;
        remove_locked(old_id);
        if (logger_) logger_->info(kModule, "stop session_id=" + old_id + " reason=replaced client=" + endpoint);
    }
    const std::shared_ptr<const Archive>& archive = archives_.at(*service_id);
    const Archive::Position position = archive->position(t);

    char id[32];
    std::snprintf(id, sizeof(id), "hs-%06llu", static_cast<unsigned long long>(next_id_++));
    RtpSessionParams params;
    params.session_id = id;
    params.client_host = *host;
    params.client_port = static_cast<uint16_t>(*port);
    params.ssrc = static_cast<uint32_t>(*ssrc);
    params.first_sequence = static_cast<uint16_t>(rng_());
    params.timestamp_base = static_cast<uint32_t>(rng_());

    auto session = std::make_unique<Session>();
    session->id = id;
    session->endpoint = endpoint;
    session->last_seen = options_.session_clock();
    session->sender = std::make_unique<RtpSender>(archive, udp_fd_, params, position, options_.sender_clock,
                                                  options_.sender_sleep, logger_);
    session->sender->start();
    sessions_[id] = std::move(session);
    by_endpoint_[endpoint] = id;
    if (logger_) {
        logger_->info(kModule, std::string("start session_id=") + id + " service=" + *service_id +
                                   " start=" + (live ? std::string("live") : std::to_string(t)) + " client=" +
                                   endpoint + " ssrc=" + std::to_string(*ssrc) + " start_time=" + std::to_string(t) +
                                   " offset=" + std::to_string(position.offset));
    }

    nlohmann::ordered_json response;
    response["session_id"] = id;
    response["service_id"] = *service_id;
    response["start_time"] = t;
    response["ssrc"] = *ssrc;
    response["payload_type"] = kRtpPayloadType;
    return json_response(200, response);
}

HttpResponse HeadEnd::keepalive(const nlohmann::json& body) {
    const std::string* id = string_field(body, "session_id");
    if (!id) return invalid("session_id missing or not a non-empty string");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(*id);
    if (it == sessions_.end()) return error_response(404, "SESSION_NOT_FOUND", "unknown, stopped or expired session");
    it->second->last_seen = options_.session_clock();
    if (logger_) logger_->info(kModule, "keepalive session_id=" + *id);
    nlohmann::ordered_json response;
    response["expires_in_s"] = options_.session_expiry.count();
    return json_response(200, response);
}

HttpResponse HeadEnd::stop(const nlohmann::json& body) {
    const std::string* id = string_field(body, "session_id");
    if (!id) return invalid("session_id missing or not a non-empty string");
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.find(*id) == sessions_.end()) {
        return error_response(404, "SESSION_NOT_FOUND", "unknown, stopped or expired session");
    }
    remove_locked(*id);  // the response is sent only after the sender thread has ended
    if (logger_) logger_->info(kModule, "stop session_id=" + *id);
    nlohmann::ordered_json response;
    response["stopped"] = true;
    return json_response(200, response);
}

}  // namespace srv
