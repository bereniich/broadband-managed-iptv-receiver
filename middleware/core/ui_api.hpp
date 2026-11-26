// AL-API: UI API facade (Sections 2.2.4, 3.6 and 4.2.4; REQ-58). Owns the semantics of IF-INT-01:
// decodes the commands received through IUiChannel, dispatches them to AL-SVC, AL-DSC and AL-SES,
// encodes one response per command and broadcasts the events session_state, stream_info and
// mw_status. Keeps the data of the last session_state for get_status (Section 4.2.3). The channel
// callbacks only post a control task (rule T1); everything else runs on the AL worker.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <json.hpp>

#include "key_manager.hpp"
#include "pal/platform.hpp"
#include "service_manager.hpp"
#include "session_controller.hpp"
#include "task_queue.hpp"

namespace al {

class UiApi {
public:
    static constexpr std::int64_t kMaxEpgRangeS = 86400;   // Section 3.2.3

    // Sets the handlers of `ui`; must be constructed before Platform::start() (Section 3.9.2).
    UiApi(pal::IUiChannel& ui, pal::ILogger& logger, TaskQueue& queue, ServiceManager& services, KeyManager& keys);

    UiApi(const UiApi&) = delete;
    UiApi& operator=(const UiApi&) = delete;

    // Composition root: AL-SES is created with session_listener() and then bound.
    void bind(SessionController& session) { session_ = &session; }
    SessionController::Listener session_listener();
    // Status handler of AL-SVC: mw_status to all clients.
    void on_mw_status(bool reachable);

    // Event data of Section 3.6.3.
    static nlohmann::json session_state_json(const SessionState& state);
    static nlohmann::json stream_info_json(const SessionState& state, const StreamDescription& stream);

private:
    void on_message(pal::IUiChannel::ClientId client, const std::string& text);
    void on_connection(pal::IUiChannel::ClientId client, bool connected);

    void get_services(pal::IUiChannel::ClientId client, std::int64_t id);
    void get_epg(pal::IUiChannel::ClientId client, std::int64_t id, const nlohmann::json& params);
    void play(pal::IUiChannel::ClientId client, std::int64_t id, const nlohmann::json& params);
    void set_key(pal::IUiChannel::ClientId client, std::int64_t id, const nlohmann::json& params);

    void respond_ok(pal::IUiChannel::ClientId client, std::int64_t id, const nlohmann::json& result);
    void respond_error(pal::IUiChannel::ClientId client, std::int64_t id, const std::string& code,
                       const std::string& message);
    void broadcast_event(const std::string& name, const nlohmann::json& data);

    pal::IUiChannel& ui_;
    pal::ILogger& logger_;
    TaskQueue& queue_;
    ServiceManager& services_;
    KeyManager& keys_;
    SessionController* session_ = nullptr;
    std::optional<nlohmann::json> last_state_;   // data of the last session_state event
    std::optional<bool> reachable_;              // nullopt until the first result of AL-SVC
};

}  // namespace al
