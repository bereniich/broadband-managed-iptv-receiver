#include "ui_api.hpp"

#include <utility>

namespace al {

namespace {

constexpr const char* kModule = "AL-API";
constexpr const char* kInvalidCommand = "INVALID_COMMAND";

using nlohmann::json;

std::string value_text(const std::string& key, const json& value) {
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        // REQ-55: a key is shown with at most its first 4 characters followed by "…".
        return key == "key" ? text.substr(0, 4) + "\xE2\x80\xA6" : text;
    }
    return value.dump();
}

// "id=7 name=play service_id=svc-1 start=live" (log keywords of the handover).
std::string command_text(std::int64_t id, const json& message) {
    std::string text = "recv command id=" + std::to_string(id);
    auto name = message.find("name");
    text += " name=" + (name != message.end() && name->is_string() ? name->get<std::string>() : std::string("?"));
    auto params = message.find("params");
    if (params != message.end() && params->is_object()) {
        for (auto it = params->begin(); it != params->end(); ++it) text += " " + it.key() + "=" + value_text(it.key(), it.value());
    }
    return text;
}

json optional_int(const std::optional<std::int64_t>& v) { return v ? json(*v) : json(nullptr); }

const char* codec(std::uint8_t stream_type) {
    switch (stream_type) {
        case 0x1B: return "H.264";
        case 0x0F: return "AAC";
        default: return "unknown";
    }
}

std::string text_or_null(const json& v) { return v.is_null() ? "null" : v.is_string() ? v.get<std::string>() : v.dump(); }

}  // namespace

UiApi::UiApi(pal::IUiChannel& ui, pal::ILogger& logger, TaskQueue& queue, ServiceManager& services, KeyManager& keys)
    : ui_(ui), logger_(logger), queue_(queue), services_(services), keys_(keys) {
    TaskQueue& q = queue_;
    ui_.set_handlers(
        [this, &q](pal::IUiChannel::ClientId client, const std::string& text) {
            q.post(TaskKind::Control, [this, client, text] { on_message(client, text); });
        },
        [this, &q](pal::IUiChannel::ClientId client, bool connected) {
            q.post(TaskKind::Control, [this, client, connected] { on_connection(client, connected); });
        });
}

SessionController::Listener UiApi::session_listener() {
    SessionController::Listener listener;
    listener.on_session_state = [this](const SessionState& s) {
        const json data = session_state_json(s);
        last_state_ = data;
        logger_.log(pal::LogLevel::Info, kModule,
                    "send event session_state session=" + std::to_string(s.session) + " state=" + to_string(s.state) +
                        " reason=" + text_or_null(data["reason"]) + " start=" + text_or_null(data["start"]) +
                        " start_time=" + text_or_null(data["start_time"]));
        broadcast_event("session_state", data);
    };
    listener.on_stream_info = [this](const SessionState& s, const StreamDescription& d) {
        const json data = stream_info_json(s, d);
        logger_.log(pal::LogLevel::Info, kModule,
                    "send event stream_info session=" + std::to_string(s.session) +
                        " program=" + std::to_string(d.program_number) + " pmt_pid=" + std::to_string(d.pmt_pid) +
                        " pcr_pid=" + std::to_string(d.pcr_pid) + " video=" + std::to_string(d.video_pid) +
                        " audio=" + (d.audio_pid ? std::to_string(*d.audio_pid) : std::string("null")));
        broadcast_event("stream_info", data);
    };
    return listener;
}

void UiApi::on_mw_status(bool reachable) {
    reachable_ = reachable;
    logger_.log(pal::LogLevel::Info, kModule, std::string("send event mw_status reachable=") + (reachable ? "true" : "false"));
    broadcast_event("mw_status", json{{"reachable", reachable}});
}

json UiApi::session_state_json(const SessionState& s) {
    json error = nullptr;
    if (s.error) error = json{{"code", s.error->code}, {"message", s.error->message}};
    return json{{"session", s.session},
                {"service_id", s.service_id},
                {"state", to_string(s.state)},
                {"reason", s.reason ? json(*s.reason) : json(nullptr)},
                {"start", s.start.live() ? json("live") : json(*s.start.time)},
                {"start_time", optional_int(s.start_time)},
                {"playing_since", optional_int(s.playing_since)},
                {"protected", s.is_protected ? json(*s.is_protected) : json(nullptr)},
                {"error", error}};
}

json UiApi::stream_info_json(const SessionState& s, const StreamDescription& d) {
    json audio = nullptr;
    if (d.audio_pid) {
        const std::uint8_t type = d.audio_stream_type.value_or(0);
        audio = json{{"pid", *d.audio_pid}, {"stream_type", type}, {"codec", codec(type)}};
    }
    return json{{"session", s.session},
                {"service_id", s.service_id},
                {"program_number", d.program_number},
                {"pmt_pid", d.pmt_pid},
                {"pcr_pid", d.pcr_pid},
                {"video", {{"pid", d.video_pid}, {"stream_type", d.video_stream_type}, {"codec", codec(d.video_stream_type)}}},
                {"audio", audio}};
}

// ---------------------------------------------------------------- receiving

void UiApi::on_connection(pal::IUiChannel::ClientId client, bool connected) {
    logger_.log(pal::LogLevel::Info, kModule,
                "client " + std::to_string(client) + (connected ? " connected" : " disconnected"));
    // Section 3.6.3: mw_status to every new client — once the reachability is known; before that the
    // client receives the first result by broadcast (decision M, step 7d).
    if (connected && reachable_) {
        ui_.send(client, json{{"type", "event"}, {"name", "mw_status"}, {"data", {{"reachable", *reachable_}}}}.dump());
    }
}

void UiApi::on_message(pal::IUiChannel::ClientId client, const std::string& text) {
    const json message = json::parse(text, nullptr, false);
    if (!message.is_object()) {
        logger_.log(pal::LogLevel::Warn, kModule, "ignored message from client " + std::to_string(client) + ": not a JSON object");
        return;
    }
    auto id_it = message.find("id");
    if (id_it == message.end() || !id_it->is_number_integer()) {
        logger_.log(pal::LogLevel::Warn, kModule, "ignored message from client " + std::to_string(client) + ": no readable id");
        return;
    }
    const std::int64_t id = id_it->get<std::int64_t>();
    logger_.log(pal::LogLevel::Info, kModule, command_text(id, message));

    auto type = message.find("type");
    auto name = message.find("name");
    auto params = message.find("params");
    if (type == message.end() || *type != "command" || name == message.end() || !name->is_string() ||
        params == message.end() || !params->is_object()) {
        respond_error(client, id, kInvalidCommand, "malformed command envelope");
        return;
    }
    const std::string command = name->get<std::string>();
    if (command == "get_services") return get_services(client, id);
    if (command == "get_epg") return get_epg(client, id, *params);
    if (command == "play") return play(client, id, *params);
    if (command == "set_key") return set_key(client, id, *params);
    if (command == "stop") {
        if (session_) session_->stop();
        return respond_ok(client, id, json::object());
    }
    if (command == "get_status") {
        return respond_ok(client, id, last_state_ ? *last_state_ : json{{"state", "idle"}});
    }
    respond_error(client, id, kInvalidCommand, "unknown command " + command);
}

// ---------------------------------------------------------------- commands (Section 3.6.2)

void UiApi::get_services(pal::IUiChannel::ClientId client, std::int64_t id) {
    if (!services_.loaded()) return respond_error(client, id, "MW_UNREACHABLE", "service list not loaded");
    json list = json::array();
    for (const ServiceEntry& e : services_.services()) {
        list.push_back({{"service_id", e.service_id}, {"number", e.number}, {"name", e.name}, {"protected", e.is_protected}});
    }
    respond_ok(client, id, json{{"services", list}});
}

void UiApi::get_epg(pal::IUiChannel::ClientId client, std::int64_t id, const json& params) {
    if (!params.contains("from") || !params["from"].is_number_integer() || !params.contains("to") ||
        !params["to"].is_number_integer()) {
        return respond_error(client, id, kInvalidCommand, "get_epg needs integer from and to");
    }
    const std::int64_t from = params["from"].get<std::int64_t>();
    const std::int64_t to = params["to"].get<std::int64_t>();
    if (from >= to || to - from > kMaxEpgRangeS) {
        logger_.log(pal::LogLevel::Warn, kModule, "get_epg INVALID_RANGE from=" + std::to_string(from) + " to=" + std::to_string(to));
        return respond_error(client, id, "INVALID_RANGE", "from < to and to - from <= 86400 required");
    }
    services_.get_epg(from, to, [this, client, id](MwError error, const json* result) {
        if (error == MwError::None && result != nullptr) return respond_ok(client, id, *result);
        respond_error(client, id, to_code(error), "EPG not available");
    });
}

void UiApi::play(pal::IUiChannel::ClientId client, std::int64_t id, const json& params) {
    auto service = params.find("service_id");
    auto start = params.find("start");
    const bool start_ok = start != params.end() && ((start->is_string() && *start == "live") || start->is_number_integer());
    if (service == params.end() || !service->is_string() || !start_ok) {
        return respond_error(client, id, kInvalidCommand, "play needs service_id and start (\"live\" or an integer)");
    }
    const std::string service_id = service->get<std::string>();
    if (services_.find(service_id) == nullptr) {   // ER-07: rejected before T-01
        return respond_error(client, id, "SERVICE_NOT_FOUND", "unknown service_id " + service_id);
    }
    StartPoint point;
    if (start->is_number_integer()) point.time = start->get<std::int64_t>();
    const std::uint64_t session = session_ ? session_->play(service_id, point) : 0;
    respond_ok(client, id, json{{"session", session}});   // before the first event of the session (T-01)
}

void UiApi::set_key(pal::IUiChannel::ClientId client, std::int64_t id, const json& params) {
    auto service = params.find("service_id");
    auto key = params.find("key");
    if (service == params.end() || !service->is_string() || key == params.end() || !key->is_string()) {
        return respond_error(client, id, kInvalidCommand, "set_key needs service_id and key");
    }
    const std::string service_id = service->get<std::string>();
    if (services_.find(service_id) == nullptr) {
        return respond_error(client, id, "SERVICE_NOT_FOUND", "unknown service_id " + service_id);
    }
    if (!keys_.store(service_id, key->get<std::string>())) {   // ER-32: nothing stored
        return respond_error(client, id, "INVALID_KEY_FORMAT", "the key must be 32 hexadecimal characters");
    }
    if (session_) session_->set_key(service_id);   // EV-SETKEY
    respond_ok(client, id, json::object());
}

// ---------------------------------------------------------------- sending

void UiApi::respond_ok(pal::IUiChannel::ClientId client, std::int64_t id, const json& result) {
    ui_.send(client, json{{"type", "response"}, {"id", id}, {"ok", true}, {"result", result}}.dump());
    logger_.log(pal::LogLevel::Debug, kModule, "send response id=" + std::to_string(id) + " ok=true");
}

void UiApi::respond_error(pal::IUiChannel::ClientId client, std::int64_t id, const std::string& code,
                          const std::string& message) {
    ui_.send(client, json{{"type", "response"}, {"id", id}, {"ok", false}, {"error", {{"code", code}, {"message", message}}}}.dump());
    logger_.log(code == kInvalidCommand ? pal::LogLevel::Warn : pal::LogLevel::Info, kModule,
                "send response id=" + std::to_string(id) + " ok=false code=" + code);
}

void UiApi::broadcast_event(const std::string& name, const json& data) {
    ui_.broadcast(json{{"type", "event"}, {"name", name}, {"data", data}}.dump());
}

}  // namespace al
