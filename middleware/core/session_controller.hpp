// AL-SES: session controller (Sections 2.2.4, 4.2.2 – 4.2.4 and 4.3; REQ-12, REQ-14, REQ-41, REQ-56,
// REQ-59). The only module that knows the order of the steps S1 – S12 of a session; it drives AL-SVC,
// AL-HEC, AL-PSI, AL-DSC and AL-OUT and the tuner, and executes the transition table of Section 4.3.4.
// Step 7c implements the clear path (T-01, T-02, T-03, T-06, T-09, T-11, T-12, T-18, T-19, T-26,
// T-27), RELEASE (rule 4) and the data path before the /start response (rule 3) for a clear stream.
// Every callback carries the generation of its attempt; tasks of another generation are discarded
// before the table is consulted (rule 2). Runs on the AL worker.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <random>
#include <string>

#include "headend_client.hpp"
#include "hls_packager.hpp"
#include "key_manager.hpp"
#include "pal/platform.hpp"
#include "psi_parser.hpp"
#include "service_manager.hpp"
#include "task_queue.hpp"

namespace al {

struct StartPoint {                         // Section 4.2.3
    std::optional<std::int64_t> time;       // nullopt = "live"
    bool live() const { return !time; }
};

enum class State { Idle, Starting, WaitingStream, KeyRequired, ValidatingKey, Playing, NoSignal, Stopped, Error };

const char* to_string(State state);   // the state values of Section 3.6.3

struct SessionState {                   // data of the session_state event (Section 3.6.3)
    std::uint64_t session = 0;
    std::string service_id;
    State state = State::Idle;
    std::optional<std::string> reason;
    StartPoint start;
    std::optional<std::int64_t> start_time;
    std::optional<std::int64_t> playing_since;
    std::optional<bool> is_protected;
    std::optional<HeadendError> error;
};

class SessionController {
public:
    static constexpr std::int64_t kArchiveWindowS = 7200;   // Section 3.1, D-08
    static constexpr std::int64_t kWindowMarginS = 5;       // REQ-41

    struct Config {   // Section 4.2.5
        std::uint16_t rtp_port = 5004;
        std::string client_host = "127.0.0.1";
        int keepalive_interval_s = 5;
        int no_data_timeout_s = 5;
        int stream_deadline_s = 3;
    };

    struct Listener {   // AL-API (observer, Section 4.2.4)
        std::function<void(const SessionState&)> on_session_state;
        std::function<void(const SessionState&, const StreamDescription&)> on_stream_info;
    };

    SessionController(pal::ITuner& tuner, pal::IClock& clock, pal::ILogger& logger, TaskQueue& queue,
                      ServiceManager& services, HeadendClient& headend, PsiParser& psi, KeyManager& keys,
                      HlsPackager& output, Config config, Listener listener,
                      std::uint32_t ssrc_seed = std::random_device{}());
    ~SessionController();

    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    // T-01 (S1): ends the current session, creates the next one and returns its number. The event
    // `starting` and the request of the service info (S2) follow as a task of their own, so that the
    // response to play is sent before the first event of the new session (Section 4.4.4).
    std::uint64_t play(const std::string& service_id, StartPoint start);
    // T-02: ends the current session (state stopped).
    void stop();

    bool has_session() const { return s_.has_value(); }
    SessionState state() const;               // data of the current session (Idle before the first)
    std::uint64_t generation() const { return s_ ? s_->generation : 0; }
    std::uint32_t ssrc() const { return s_ ? s_->ssrc : 0; }

    // REQ-41: max(T, now - 7195); "live" if T >= now.
    static StartPoint clamp(StartPoint start, std::int64_t now);

private:
    enum class Decision { Unknown, Clear, Protected };   // protection recorded by the data path
    enum class Timer { Keepalive, NoData, Deadline };

    struct Session {
        std::uint64_t number = 0;
        std::uint64_t generation = 0;
        std::string service_id;
        StartPoint start;
        std::uint32_t ssrc = 0;
        std::string stream_control_url;
        std::string headend_session_id;   // empty: no head-end session
        std::optional<std::int64_t> start_time;
        State state = State::Starting;
        std::optional<std::string> reason;
        std::optional<HeadendError> error;
        std::optional<bool> is_protected;
        std::optional<StreamDescription> stream;
        bool stream_info_sent = false;
        // Data-path flags (Section 4.3.1, rule 3).
        bool locked = false;
        std::int64_t locked_ms = 0;
        Decision protection = Decision::Unknown;
        bool first_segment_listed = false;
        std::optional<std::int64_t> playing_since;
        int keepalive_failures = 0;
    };

    bool current(std::uint64_t generation) const { return s_ && s_->generation == generation; }
    bool in(std::initializer_list<State> states) const;
    void begin(std::uint64_t generation);                       // S1 continuation: event, S2
    void on_info(std::uint64_t generation, MwError error, const ServiceInfo* info);
    void send_start();                                          // S4
    void on_start(std::uint64_t generation, const HeadendError& error, const StartResult* result);
    void on_tuner(std::uint64_t generation, pal::TunerStatus status);
    void on_pat(std::uint64_t generation, std::uint16_t pmt_pid);
    void on_pmt(std::uint64_t generation, const StreamDescription& stream);
    void on_video1(std::uint64_t generation, al::Protection protection);
    void on_first_segment(std::uint64_t generation);
    void on_timer(std::uint64_t generation, Timer timer);
    void on_keepalive(std::uint64_t generation, const HeadendError& error);

    void release();                                             // rule 4
    void start_timer(Timer timer, int seconds);
    void cancel_timer(Timer timer);
    std::optional<pal::IClock::TimerId>& timer_slot(Timer timer);
    void set_state(State state, std::optional<std::string> reason = std::nullopt);
    void emit_state();
    void not_handled(const std::string& event);                // events of later steps (rule 2: Debug)
    void log(pal::LogLevel level, const std::string& message);

    pal::ITuner& tuner_;
    pal::IClock& clock_;
    pal::ILogger& logger_;
    TaskQueue& queue_;
    ServiceManager& services_;
    HeadendClient& headend_;
    PsiParser& psi_;
    KeyManager& keys_;
    HlsPackager& output_;
    Config config_;
    Listener listener_;
    std::mt19937 rng_;

    std::optional<Session> s_;
    std::uint64_t next_number_ = 1;
    std::uint64_t next_generation_ = 1;
    std::atomic<std::uint64_t> tuned_generation_{0};   // read by the tuner status handler (Section 4.2.2)

    std::optional<ServiceManager::Token> info_token_;
    std::optional<HeadendClient::Token> start_token_;
    std::optional<HeadendClient::Token> keepalive_token_;
    std::optional<pal::IClock::TimerId> keepalive_timer_;
    std::optional<pal::IClock::TimerId> no_data_timer_;
    std::optional<pal::IClock::TimerId> deadline_timer_;
};

}  // namespace al
