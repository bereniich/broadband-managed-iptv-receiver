#include "session_controller.hpp"

#include <algorithm>
#include <utility>

namespace al {

namespace {

constexpr const char* kModule = "AL-SES";

std::string start_text(const StartPoint& start) { return start.live() ? "live" : std::to_string(*start.time); }

}  // namespace

const char* to_string(State state) {
    switch (state) {
        case State::Idle: return "idle";
        case State::Starting: return "starting";
        case State::WaitingStream: return "waiting_stream";
        case State::KeyRequired: return "key_required";
        case State::ValidatingKey: return "validating_key";
        case State::Playing: return "playing";
        case State::NoSignal: return "no_signal";
        case State::Stopped: return "stopped";
        case State::Error: return "error";
    }
    return "unknown";
}

SessionController::SessionController(pal::ITuner& tuner, pal::IClock& clock, pal::ILogger& logger, TaskQueue& queue,
                                     ServiceManager& services, HeadendClient& headend, PsiParser& psi,
                                     KeyManager& keys, HlsPackager& output, Config config, Listener listener,
                                     std::uint32_t ssrc_seed)
    : tuner_(tuner), clock_(clock), logger_(logger), queue_(queue), services_(services), headend_(headend),
      psi_(psi), keys_(keys), output_(output), config_(std::move(config)), listener_(std::move(listener)),
      rng_(ssrc_seed) {
    // Set once, before Platform::start() (Section 3.9.2). It cannot capture a session, so it reads the
    // generation of the current tuning (Section 4.2.2) and only posts a task (rule T1).
    TaskQueue& q = queue_;
    tuner_.set_status_handler([this, &q](pal::TunerStatus status) {
        const std::uint64_t g = tuned_generation_.load();
        q.post(TaskKind::Control, [this, g, status] { on_tuner(g, status); });
    });
}

SessionController::~SessionController() {
    for (Timer t : {Timer::Keepalive, Timer::NoData, Timer::Deadline}) cancel_timer(t);
}

StartPoint SessionController::clamp(StartPoint start, std::int64_t now) {
    if (start.live() || *start.time >= now) return StartPoint{};
    const std::int64_t oldest = now - kArchiveWindowS + kWindowMarginS;
    return StartPoint{std::max(*start.time, oldest)};
}

SessionState SessionController::state() const {
    SessionState d;
    if (!s_) return d;
    d.session = s_->number;
    d.service_id = s_->service_id;
    d.state = s_->state;
    d.reason = s_->reason;
    d.start = s_->start;
    d.start_time = s_->start_time;
    d.playing_since = s_->playing_since;
    d.is_protected = s_->is_protected;
    d.error = s_->error;
    return d;
}

bool SessionController::in(std::initializer_list<State> states) const {
    if (!s_) return false;
    for (State st : states) {
        if (s_->state == st) return true;
    }
    return false;
}

// ---------------------------------------------------------------- T-01, T-02

std::uint64_t SessionController::play(const std::string& service_id, StartPoint start) {
    // T-01: a session in an active state, key_required or error is released and reported stopped.
    if (in({State::Starting, State::WaitingStream, State::ValidatingKey, State::Playing, State::NoSignal,
            State::KeyRequired, State::Error})) {
        release();
        set_state(State::Stopped);
    }
    Session s;
    s.number = next_number_++;
    s.generation = next_generation_++;
    s.service_id = service_id;
    s.start = clamp(start, clock_.unix_time_s());
    const std::uint32_t previous = s_ ? s_->ssrc : 0;
    do {
        s.ssrc = static_cast<std::uint32_t>(rng_());
    } while (s_ && s.ssrc == previous);
    s.state = State::Starting;
    s_ = std::move(s);
    log(pal::LogLevel::Info, "play service=" + service_id + " start=" + start_text(s_->start));

    const std::uint64_t g = s_->generation;
    queue_.post(TaskKind::Control, [this, g] { begin(g); });
    return s_->number;
}

void SessionController::stop() {
    if (!in({State::Starting, State::WaitingStream, State::ValidatingKey, State::Playing, State::NoSignal,
             State::KeyRequired, State::Error})) {
        return;
    }
    release();   // nothing left to release in key_required and error
    set_state(State::Stopped);
}

void SessionController::set_key(const std::string& service_id) {
    const bool same = s_ && s_->service_id == service_id;
    if (same && s_->state == State::KeyRequired) {
        not_handled("EV-SETKEY in key_required");   // T-24, step 11
        return;
    }
    if (same && in({State::Starting, State::WaitingStream, State::ValidatingKey}) && s_->stream &&
        s_->protection != Decision::Clear) {
        not_handled("EV-SETKEY after EV-PMT");   // T-33, step 11
        return;
    }
    // T-25: nothing besides the storing by AL-DSC; the key is used at the next PMT of that service.
    log(pal::LogLevel::Debug, "key of " + service_id + " stored, used at the next PMT of the service");
}

// ---------------------------------------------------------------- S1 continuation, S2, T-03, S4

void SessionController::begin(std::uint64_t g) {
    if (!current(g) || s_->state != State::Starting) return;
    log(pal::LogLevel::Info, std::string("state ") + to_string(State::Starting));
    emit_state();
    info_token_ = services_.get_service_info(
        s_->service_id, [this, g](MwError error, const ServiceInfo* info) { on_info(g, error, info); });
}

void SessionController::on_info(std::uint64_t g, MwError error, const ServiceInfo* info) {
    if (!current(g)) return;
    info_token_.reset();
    if (s_->state != State::Starting) return;
    if (error != MwError::None || info == nullptr) {
        not_handled(std::string("EV-INFO-ERR ") + to_code(error));   // T-05, step 9
        return;
    }
    // T-03, S3: tuning, the HLS directory and the PAT filter before the start request.
    s_->stream_control_url = info->stream_control_url;
    tuner_.untune();
    tuned_generation_.store(g);
    if (!tuner_.tune(pal::TuneParams{config_.rtp_port, s_->ssrc})) {
        not_handled("EV-TUNE-FAIL");   // T-04, step 9
        return;
    }
    HlsPackager::Events out;
    out.on_first_segment = [this, g] { on_first_segment(g); };
    out.on_error = [this] { not_handled("EV-OUT-ERR"); };   // T-30, step 9
    if (!output_.open(s_->number, g, out)) {
        not_handled("EV-OUT-ERR");
        return;
    }
    PsiParser::Events psi;
    psi.on_pat = [this, g](std::uint16_t pid) { on_pat(g, pid); };
    psi.on_pmt = [this, g](const StreamDescription& d) { on_pmt(g, d); };
    psi.on_pmt_novideo = [this] { not_handled("EV-PMT-NOVIDEO"); };   // T-13, step 9
    if (!psi_.start_pat(s_->number, g, psi)) {
        not_handled("PAT filter not opened");   // P2 (RECEIVER_ERROR), step 9
        return;
    }
    send_start();
}

void SessionController::send_start() {
    StartRequest request;
    request.service_id = s_->service_id;
    request.start = s_->start.time;
    request.client_host = config_.client_host;
    request.client_port = config_.rtp_port;
    request.ssrc = s_->ssrc;
    const std::uint64_t g = s_->generation;
    start_token_ = headend_.start(s_->number, s_->stream_control_url, request,
                                  [this, g](const HeadendError& e, const StartResult* r) { on_start(g, e, r); });
}

// ---------------------------------------------------------------- T-06

void SessionController::on_start(std::uint64_t g, const HeadendError& error, const StartResult* result) {
    if (!current(g)) return;
    start_token_.reset();
    if (s_->state != State::Starting) return;
    if (!error.ok() || result == nullptr) {
        not_handled("EV-START-ERR " + error.code);   // T-07, T-08, step 9
        return;
    }
    s_->headend_session_id = result->session_id;
    s_->start_time = result->start_time;
    s_->keepalive_failures = 0;
    start_timer(Timer::Keepalive, config_.keepalive_interval_s);
    if (!s_->locked) start_timer(Timer::NoData, config_.no_data_timeout_s);

    // Rule 3: the deferred part of the recorded data-path flags; only the resulting state is sent.
    if (s_->protection == Decision::Clear) s_->is_protected = false;   // rest of T-19 already done
    if (s_->protection == Decision::Clear && s_->first_segment_listed) {
        s_->playing_since = clock_.unix_time_s();   // as T-18
        set_state(State::Playing);
        return;
    }
    set_state(State::WaitingStream);
}

// ---------------------------------------------------------------- tuner, T-09

void SessionController::on_tuner(std::uint64_t g, pal::TunerStatus status) {
    if (!current(g)) {
        log(pal::LogLevel::Debug, "discarded tuner status of generation " + std::to_string(g));
        return;
    }
    const bool locked = status == pal::TunerStatus::Locked;
    log(pal::LogLevel::Info, std::string("tuner ") + (locked ? "Locked" : "Unlocked"));
    if (locked && in({State::Starting, State::WaitingStream}) && !s_->locked) {
        // T-09 (S6): the first Locked of the attempt.
        cancel_timer(Timer::NoData);
        s_->locked = true;
        s_->locked_ms = clock_.monotonic_ms();
        start_timer(Timer::Deadline, config_.stream_deadline_s);
        return;
    }
    not_handled(locked ? "EV-LOCKED" : "EV-UNLOCKED");   // T-10, T-22, T-23, T-32, step 9
}

// ---------------------------------------------------------------- T-11, T-12

void SessionController::on_pat(std::uint64_t g, std::uint16_t pmt_pid) {
    if (!current(g) || !in({State::Starting, State::WaitingStream})) return;
    if (!psi_.start_pmt(pmt_pid)) not_handled("PMT filter not opened");   // P2, step 9
}

void SessionController::on_pmt(std::uint64_t g, const StreamDescription& stream) {
    if (!current(g) || !in({State::Starting, State::WaitingStream})) return;
    // T-12 (S8).
    s_->stream = stream;
    if (!s_->stream_info_sent) {
        s_->stream_info_sent = true;   // REQ-18: once per session, not again after a restart
        if (listener_.on_stream_info) listener_.on_stream_info(state(), stream);
    }
    output_.set_stream(stream);
    KeyManager::Events dsc;
    dsc.on_video1 = [this, g](al::Protection p) { on_video1(g, p); };
    dsc.on_key_ok = [this] { not_handled("EV-KEY-OK"); };     // T-16, step 11
    dsc.on_key_bad = [this] { not_handled("EV-KEY-BAD"); };   // T-17, step 11
    dsc.on_packet = [this, g](const std::uint8_t* packet, bool) {
        if (current(g)) output_.feed(packet);   // S11
    };
    // REQ-49: the stored key is set before the TS filters are started (inside KeyManager::start).
    if (!keys_.start(s_->number, g, s_->service_id, stream, dsc)) not_handled("TS filters not opened");   // P2
}

// ---------------------------------------------------------------- T-19, T-18

void SessionController::on_video1(std::uint64_t g, al::Protection protection) {
    if (!current(g) || !in({State::Starting, State::WaitingStream})) return;
    if (protection != al::Protection::Clear) {
        s_->protection = Decision::Protected;
        not_handled("EV-VIDEO1 protected");   // T-14, T-15, step 11
        return;
    }
    // T-19: data-path actions now (also in starting, rule 3); protected = false is deferred in starting.
    s_->protection = Decision::Clear;
    cancel_timer(Timer::Deadline);
    output_.set_write_enabled(true);
    if (s_->state == State::WaitingStream) s_->is_protected = false;
}

void SessionController::on_first_segment(std::uint64_t g) {
    if (!current(g)) return;
    if (s_->state == State::Starting) {
        s_->first_segment_listed = true;   // rule 3: applied by T-06
        return;
    }
    if (s_->state == State::WaitingStream && s_->protection == Decision::Clear) {
        s_->playing_since = clock_.unix_time_s();   // T-18 (S12)
        set_state(State::Playing);
        return;
    }
    not_handled("EV-SEG1");   // validating_key: step 11
}

// ---------------------------------------------------------------- timers, T-26, T-27

void SessionController::on_timer(std::uint64_t g, Timer timer) {
    if (!current(g)) return;
    timer_slot(timer).reset();
    if (timer == Timer::Keepalive &&
        in({State::WaitingStream, State::ValidatingKey, State::Playing, State::NoSignal})) {
        // T-26: POST /keepalive; restart the keepalive timer.
        start_timer(Timer::Keepalive, config_.keepalive_interval_s);
        keepalive_token_ = headend_.keepalive(s_->stream_control_url, s_->headend_session_id,
                                              [this, g](const HeadendError& e) { on_keepalive(g, e); });
        return;
    }
    not_handled(timer == Timer::NoData ? "EV-T-NODATA" : timer == Timer::Deadline ? "EV-T-DEADLINE" : "EV-T-KA");
}

void SessionController::on_keepalive(std::uint64_t g, const HeadendError& error) {
    if (!current(g)) return;
    keepalive_token_.reset();
    if (!in({State::WaitingStream, State::ValidatingKey, State::Playing, State::NoSignal})) return;
    if (error.ok()) {
        s_->keepalive_failures = 0;   // T-27
        return;
    }
    not_handled("EV-KA-FAIL " + error.code);   // T-28, T-29, step 9
}

// ---------------------------------------------------------------- RELEASE (rule 4)

void SessionController::release() {
    // 1. Timers and pending HTTP requests of the attempt.
    for (Timer t : {Timer::Keepalive, Timer::NoData, Timer::Deadline}) cancel_timer(t);
    if (info_token_) services_.cancel(*info_token_);
    if (start_token_) headend_.cancel(*start_token_);
    if (keepalive_token_) headend_.cancel(*keepalive_token_);
    info_token_.reset();
    start_token_.reset();
    keepalive_token_.reset();
    // 2. Demux filters, then the descrambler keys (KeyManager::close ends with clear_keys).
    psi_.close();
    keys_.close();
    // 3. Tuner. 4. AL-OUT (held data discarded, close_session).
    tuner_.untune();
    output_.close();
    // 5. POST /stop for the head-end session; its response is only logged.
    if (s_ && !s_->headend_session_id.empty()) {
        headend_.stop(s_->stream_control_url, s_->headend_session_id, nullptr);
        s_->headend_session_id.clear();
    }
}

// ---------------------------------------------------------------- helpers

std::optional<pal::IClock::TimerId>& SessionController::timer_slot(Timer timer) {
    switch (timer) {
        case Timer::Keepalive: return keepalive_timer_;
        case Timer::NoData: return no_data_timer_;
        case Timer::Deadline: break;
    }
    return deadline_timer_;
}

void SessionController::start_timer(Timer timer, int seconds) {
    cancel_timer(timer);
    const std::uint64_t g = s_->generation;
    TaskQueue& q = queue_;
    timer_slot(timer) = clock_.start_timer(seconds * 1000, [this, &q, g, timer] {
        q.post(TaskKind::Control, [this, g, timer] { on_timer(g, timer); });
    });
}

void SessionController::cancel_timer(Timer timer) {
    std::optional<pal::IClock::TimerId>& slot = timer_slot(timer);
    if (slot) clock_.cancel_timer(*slot);
    slot.reset();
}

void SessionController::set_state(State state, std::optional<std::string> reason) {
    const State previous = s_->state;
    s_->state = state;
    s_->reason = std::move(reason);
    log(pal::LogLevel::Info, std::string("state ") + to_string(previous) + " -> " + to_string(state));
    emit_state();
}

void SessionController::emit_state() {
    if (listener_.on_session_state) listener_.on_session_state(state());
}

void SessionController::not_handled(const std::string& event) {
    log(pal::LogLevel::Debug, "event not handled in state " + std::string(s_ ? to_string(s_->state) : "idle") +
                                  ": " + event);
}

void SessionController::log(pal::LogLevel level, const std::string& message) {
    std::string suffix;
    if (s_) suffix = " session=" + std::to_string(s_->number) + " generation=" + std::to_string(s_->generation);
    logger_.log(level, kModule, message + suffix);
}

}  // namespace al
