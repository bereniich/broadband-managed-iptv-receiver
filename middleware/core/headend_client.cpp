#include "headend_client.hpp"

#include <limits>
#include <utility>

#include <json.hpp>

namespace al {

namespace {

constexpr const char* kModule = "AL-HEC";

using nlohmann::json;

json parse(const pal::HttpResponse& r) {
    return r.transport_ok ? json::parse(r.body, nullptr, false) : json(json::value_t::discarded);
}

// The error object of a 4xx response ({"error": {"code", "message"}}), if it is valid.
std::optional<HeadendError> error_object(const pal::HttpResponse& r, const json& body) {
    if (!r.transport_ok || r.status < 400 || r.status > 499 || !body.is_object()) return std::nullopt;
    auto error = body.find("error");
    if (error == body.end() || !error->is_object()) return std::nullopt;
    auto code = error->find("code");
    if (code == error->end() || !code->is_string() || code->get<std::string>().empty()) return std::nullopt;
    auto message = error->find("message");
    return HeadendError{code->get<std::string>(),
                        message != error->end() && message->is_string() ? message->get<std::string>() : ""};
}

HeadendError unreachable(const pal::HttpResponse& r, const char* request) {
    std::string why;
    if (!r.transport_ok) {
        why = r.error.empty() ? "transport failure" : r.error;
    } else if (r.status == 200) {
        why = "invalid response body";
    } else {
        why = "status " + std::to_string(r.status);
    }
    return HeadendError{kHeadendUnreachable, std::string("head-end not reachable (") + request + "): " + why};
}

bool is_string(const json& o, const char* f) { return o.contains(f) && o[f].is_string(); }
bool is_integer(const json& o, const char* f) { return o.contains(f) && o[f].is_number_integer(); }

// All fields of the response of Section 3.3.1 are mandatory (Section 3.1).
std::optional<StartResult> start_result(const json& body) {
    if (!body.is_object() || !is_string(body, "session_id") || body["session_id"].get<std::string>().empty() ||
        !is_string(body, "service_id") || !is_integer(body, "start_time") || !is_integer(body, "ssrc") ||
        !is_integer(body, "payload_type")) {
        return std::nullopt;
    }
    const std::int64_t ssrc = body["ssrc"].get<std::int64_t>();
    if (ssrc < 0 || ssrc > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    StartResult result;
    result.session_id = body["session_id"].get<std::string>();
    result.start_time = body["start_time"].get<std::int64_t>();
    result.ssrc = static_cast<std::uint32_t>(ssrc);
    return result;
}

}  // namespace

HeadendClient::HeadendClient(pal::IHttpClient& http, pal::ILogger& logger, TaskQueue& queue, int timeout_ms)
    : http_(http), logger_(logger), queue_(queue), timeout_ms_(timeout_ms) {}

HeadendClient::~HeadendClient() {
    for (const auto& entry : pending_) {
        if (entry.second.http) http_.cancel(*entry.second.http);
    }
}

HeadendClient::Token HeadendClient::start(std::uint64_t session, const std::string& url, const StartRequest& request,
                                          StartDone done) {
    json body = {{"service_id", request.service_id},
                 {"client_host", request.client_host},
                 {"client_port", request.client_port},
                 {"ssrc", request.ssrc}};
    body["start"] = request.start ? json(*request.start) : json("live");
    Pending p{Kind::Start, std::nullopt, session, "", std::move(done), nullptr};
    return send(Kind::Start, url + "/start", body.dump(), std::move(p));
}

HeadendClient::Token HeadendClient::keepalive(const std::string& url, const std::string& session_id, Done done) {
    Pending p{Kind::Keepalive, std::nullopt, 0, session_id, nullptr, std::move(done)};
    return send(Kind::Keepalive, url + "/keepalive", json{{"session_id", session_id}}.dump(), std::move(p));
}

HeadendClient::Token HeadendClient::stop(const std::string& url, const std::string& session_id, Done done) {
    logger_.log(pal::LogLevel::Info, kModule, "stop headend_session=" + session_id);
    Pending p{Kind::Stop, std::nullopt, 0, session_id, nullptr, std::move(done)};
    return send(Kind::Stop, url + "/stop", json{{"session_id", session_id}}.dump(), std::move(p));
}

void HeadendClient::cancel(Token token) {
    auto it = pending_.find(token);
    if (it == pending_.end()) return;
    if (it->second.http) http_.cancel(*it->second.http);
    pending_.erase(it);
}

HeadendClient::Token HeadendClient::send(Kind, const std::string& url, const std::string& body, Pending pending) {
    const Token token = next_token_++;
    pending_.emplace(token, std::move(pending));
    TaskQueue& queue = queue_;
    pending_.at(token).http = http_.post_json(url, body, timeout_ms_, [this, &queue, token](const pal::HttpResponse& r) {
        queue.post(TaskKind::Control, [this, token, r] { on_response(token, r); });
    });
    return token;
}

void HeadendClient::on_response(Token token, const pal::HttpResponse& r) {
    auto it = pending_.find(token);
    if (it == pending_.end()) return;   // cancelled
    Pending p = std::move(it->second);
    pending_.erase(it);
    const json body = parse(r);
    const std::optional<HeadendError> error = error_object(r, body);

    switch (p.kind) {
        case Kind::Start: {
            if (r.transport_ok && r.status == 200) {
                if (std::optional<StartResult> result = start_result(body)) {
                    logger_.log(pal::LogLevel::Info, kModule,
                                "start ok session=" + std::to_string(p.session) + " headend_session=" +
                                    result->session_id + " ssrc=" + std::to_string(result->ssrc) +
                                    " start_time=" + std::to_string(result->start_time));
                    if (p.start_done) p.start_done(HeadendError{}, &*result);
                    return;
                }
            }
            const HeadendError e = error ? *error : unreachable(r, "start");
            // INVALID_REQUEST is a programming error of the receiver (ER-13).
            logger_.log(e.code == "INVALID_REQUEST" ? pal::LogLevel::Error : pal::LogLevel::Warn, kModule,
                        "start failed session=" + std::to_string(p.session) + " code=" + e.code + " " + e.message);
            if (p.start_done) p.start_done(e, nullptr);
            return;
        }
        case Kind::Keepalive: {
            if (r.transport_ok && r.status == 200 && is_integer(body, "expires_in_s")) {
                logger_.log(pal::LogLevel::Debug, kModule, "keepalive ok headend_session=" + p.session_id);
                if (p.done) p.done(HeadendError{});
                return;
            }
            HeadendError e = unreachable(r, "keepalive");
            if (error && error->code == kSessionNotFound) e = *error;
            logger_.log(pal::LogLevel::Warn, kModule,
                        "keepalive failed headend_session=" + p.session_id + " code=" + e.code + " " + e.message);
            if (p.done) p.done(e);
            return;
        }
        case Kind::Stop: {
            const bool stopped = r.transport_ok && r.status == 200 && body.is_object() && body.contains("stopped") &&
                                 body["stopped"].is_boolean();
            if (stopped || (error && error->code == kSessionNotFound)) {   // Section 3.3.2
                logger_.log(pal::LogLevel::Info, kModule, "stop ok headend_session=" + p.session_id);
                if (p.done) p.done(HeadendError{});
                return;
            }
            const HeadendError e = unreachable(r, "stop");
            logger_.log(pal::LogLevel::Warn, kModule,
                        "stop failed headend_session=" + p.session_id + " code=" + e.code + " " + e.message);
            if (p.done) p.done(e);
            return;
        }
    }
}

}  // namespace al
