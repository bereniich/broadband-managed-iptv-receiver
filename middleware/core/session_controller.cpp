#include "session_controller.hpp"

#include <algorithm>
#include <utility>

namespace al {

namespace {

constexpr const char* kModule = "AL-SES";
constexpr const char* kFutureTime = "FUTURE_TIME";            // Section 3.10
constexpr const char* kInvalidStream = "INVALID_STREAM";      // Section 3.10
constexpr const char* kReceiverError = "RECEIVER_ERROR";      // Section 3.10
constexpr const char* kNoDataAtStart = "no_data_at_start";    // Section 3.6.3
constexpr const char* kDataLost = "data_lost";                // Section 3.6.3
constexpr const char* kNoKey = "no_key";                      // Section 3.6.3
constexpr const char* kKeyRejected = "key_rejected";          // Section 3.6.3

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

SessionController::SessionController(pal::ITuner& tuner, pal::IDemux& demux, pal::IClock& clock, pal::ILogger& logger, TaskQueue& queue,
                                     ServiceManager& services, HeadendClient& headend, PsiParser& psi,
                                     KeyManager& keys, HlsPackager& output, Config config, Listener listener,
                                     std::uint32_t ssrc_seed)
    : tuner_(tuner), demux_(demux), clock_(clock), logger_(logger), queue_(queue), services_(services), headend_(headend),
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
    for (Timer t : {Timer::Keepalive, Timer::NoData, Timer::Deadline, Timer::Stats}) cancel_timer(t);
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
    // T-01: a session in an active state, key_required or error is released and reported stopped;
    // key_required and error own no PAL resources any more (rule 5), so nothing is called for them.
    if (in({State::Starting, State::WaitingStream, State::ValidatingKey, State::Playing, State::NoSignal})) {
        release();
        set_state(State::Stopped);
    } else if (in({State::KeyRequired, State::Error})) {
        set_state(State::Stopped);
    }
    Session s;
    s.number = next_number_++;
    s.generation = next_generation_++;
    s.service_id = service_id;
    s.start = clamp(start, clock_.unix_time_s());
    s.ssrc = new_ssrc();
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
    // T-02: RELEASE; nothing left to release in key_required and error (rule 5), so no PAL call there.
    if (!in({State::KeyRequired, State::Error})) release();
    set_state(State::Stopped);
}

void SessionController::shutdown(std::function<void()> done) {
    if (!in({State::Starting, State::WaitingStream, State::ValidatingKey, State::Playing, State::NoSignal})) {
        if (done) done();   // key_required, error, stopped and idle own no resources (rule 5)
        return;
    }
    log(pal::LogLevel::Info, "shutdown");
    // T-31: RELEASE including POST /stop; AL-MAIN waits for its response or its timeout.
    const bool stop_sent = release([this, done](const HeadendError& e) {
        logger_.log(pal::LogLevel::Info, kModule, std::string("shutdown: /stop ") + (e.ok() ? "answered" : e.code));
        if (done) done();
    });
    set_state(State::Stopped);
    if (!stop_sent && done) done();
}

void SessionController::set_key(const std::string& service_id) {
    const bool same = s_ && s_->service_id == service_id;
    if (same && s_->state == State::KeyRequired) {
        log(pal::LogLevel::Info, "set_key in key_required: restart from the kept start point (T-24)");
        restart();
        return;
    }
    if (same && in({State::Starting, State::WaitingStream, State::ValidatingKey}) && s_->stream &&
        s_->protection != Decision::Clear && s_->key_result != KeyResult::Accepted) {
        // T-33: the TS filters of the attempt were started at T-12 with the previous key or without a
        // key; RELEASE (the stored key, now the new one, is kept), then as T-24.
        log(pal::LogLevel::Info, "set_key after EV-PMT, key not yet accepted: restart with the new key (T-33)");
        release();
        restart();
        return;
    }
    // T-25: nothing besides the storing by AL-DSC; the key is used at the next PMT of that service.
    log(pal::LogLevel::Debug, "key of " + service_id + " stored, used at the next PMT of the service");
}

void SessionController::restart() {
    // T-24 (REQ-49, D-14): same number and start point, stream_info_sent kept (REQ-18); a new generation
    // and SSRC; everything else reset (protected again null, Section 3.6.3). Then S2 as after play.
    Session s;
    s.number = s_->number;
    s.generation = next_generation_++;
    s.service_id = s_->service_id;
    s.start = s_->start;
    s.stream_info_sent = s_->stream_info_sent;
    s.ssrc = new_ssrc();
    s.state = State::Starting;
    s_ = std::move(s);
    const std::uint64_t g = s_->generation;
    queue_.post(TaskKind::Control, [this, g] { begin(g); });
}

std::uint32_t SessionController::new_ssrc() {
    const std::uint32_t previous = s_ ? s_->ssrc : 0;
    std::uint32_t ssrc;
    do {
        ssrc = static_cast<std::uint32_t>(rng_());
    } while (s_ && ssrc == previous);
    return ssrc;
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
        // T-05: MW_UNREACHABLE or SERVICE_NOT_FOUND.
        const MwError code = error != MwError::None ? error : MwError::MwUnreachable;
        fail(to_code(code), "no service info for " + s_->service_id);
        return;
    }
    // T-03, S3: tuning, the HLS directory and the PAT filter before the start request.
    s_->stream_control_url = info->stream_control_url;
    tuner_.untune();
    tuned_generation_.store(g);
    if (!tuner_.tune(pal::TuneParams{config_.rtp_port, s_->ssrc})) {
        fail(kReceiverError, "the tuner cannot open UDP port " + std::to_string(config_.rtp_port));   // T-04
        return;
    }
    start_timer(Timer::Stats, kStatsIntervalS);   // while the attempt owns the tuner; RELEASE cancels it
    // The handlers below run inside AL-OUT and AL-PSI, which allow close() from a handler.
    HlsPackager::Events out;
    out.on_first_segment = [this, g] { on_first_segment(g); };
    out.on_error = [this, g] { on_output_error(g); };
    if (!output_.open(s_->number, g, out)) {
        fail(kReceiverError, "the HLS directory cannot be created");   // T-30 (EV-OUT-ERR at S3)
        return;
    }
    PsiParser::Events psi;
    psi.on_pat = [this, g](std::uint16_t pid) { on_pat(g, pid); };
    psi.on_pmt = [this, g](const StreamDescription& d) { on_pmt(g, d); };
    psi.on_pmt_novideo = [this, g] { on_pmt_novideo(g); };
    if (!psi_.start_pat(s_->number, g, psi)) {
        fail(kReceiverError, "the PAT section filter cannot be opened");   // issue #24
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
        if (error.code == kFutureTime && !s_->start.live()) {
            // T-07: the start point becomes "live" and /start is sent again (Section 3.10, ER-11).
            log(pal::LogLevel::Info, "start " + start_text(s_->start) + " answered FUTURE_TIME, retry as live");
            s_->start = StartPoint{};
            send_start();
            return;
        }
        // T-08: the code of the head-end, HEADEND_UNREACHABLE on a transport failure (AL-HEC).
        fail(error.ok() ? kHeadendUnreachable : error.code, "POST /start failed: " + error.message);
        return;
    }
    s_->headend_session_id = result->session_id;
    s_->start_time = result->start_time;
    s_->keepalive_failures = 0;
    start_timer(Timer::Keepalive, config_.keepalive_interval_s);
    if (!s_->locked) start_timer(Timer::NoData, config_.no_data_timeout_s);

    // Rule 3: the deferred part of the recorded data-path flags, in the order of T-06; only the
    // resulting state is sent.
    if (s_->protection == Decision::Clear) {
        s_->is_protected = false;   // the rest of T-19 was already done
        if (s_->first_segment_listed) {
            s_->playing_since = clock_.unix_time_s();   // as T-18
            set_state(State::Playing);
            return;
        }
    } else if (s_->protection == Decision::Protected) {
        s_->is_protected = true;
        if (!s_->key_set) {
            release();   // as T-14 (REQ-45), with POST /stop for the session id just stored
            set_state(State::KeyRequired, kNoKey);
            return;
        }
        // As T-15; then a recorded rejection as T-17, or an accepted key (T-16 already done).
        if (s_->key_result == KeyResult::Rejected) {
            reject_key();
            return;
        }
        if (s_->key_result == KeyResult::Accepted && s_->first_segment_listed) {
            s_->playing_since = clock_.unix_time_s();   // as T-18
            set_state(State::Playing);
            return;
        }
        set_state(State::ValidatingKey);
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
    const bool no_data_at_start = s_->state == State::NoSignal && s_->reason == kNoDataAtStart;
    if (locked && in({State::Starting, State::WaitingStream}) && !s_->locked) {
        first_locked();   // T-09
        return;
    }
    if (locked && no_data_at_start) {
        first_locked();   // T-10: as T-09, then back to waiting_stream
        set_state(State::WaitingStream);
        return;
    }
    if (locked && s_->state == State::NoSignal && s_->reason == kDataLost) {
        set_state(State::Playing);   // T-23: AL-OUT continues with the next key-frame start
        return;
    }
    if (!locked && s_->state == State::Playing) {
        set_state(State::NoSignal, kDataLost);   // T-22: the player and the HLS files are kept
        return;
    }
    if (!locked && (in({State::Starting, State::WaitingStream, State::ValidatingKey}) || no_data_at_start)) {
        // T-32: the stream deadline or the no-data timer decides (T-20, T-21).
        log(pal::LogLevel::Debug, "Unlocked ignored (T-32)");
        return;
    }
    not_handled(locked ? "EV-LOCKED" : "EV-UNLOCKED");
}

void SessionController::first_locked() {
    // S6: cancel the no-data timer; the stream deadline starts at the first Locked of the attempt.
    cancel_timer(Timer::NoData);
    s_->locked = true;
    s_->locked_ms = clock_.monotonic_ms();
    start_timer(Timer::Deadline, config_.stream_deadline_s);
}

// ---------------------------------------------------------------- T-11, T-12

void SessionController::on_pat(std::uint64_t g, std::uint16_t pmt_pid) {
    if (!current(g) || !in({State::Starting, State::WaitingStream})) return;
    if (!psi_.start_pmt(pmt_pid)) {
        fail(kReceiverError, "the PMT section filter cannot be opened");   // issue #24
    }
}

void SessionController::on_pmt_novideo(std::uint64_t g) {
    if (!current(g) || !in({State::Starting, State::WaitingStream})) return;
    fail(kInvalidStream, "the PMT lists no video stream (stream_type 0x1B)");   // T-13 (REQ-17)
}

void SessionController::on_output_error(std::uint64_t g) {
    if (!current(g) || !in({State::Starting, State::WaitingStream, State::ValidatingKey, State::Playing})) return;
    fail(kReceiverError, "an HLS file cannot be written");   // T-30
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
    dsc.on_key_ok = [this, g] { on_key_ok(g); };
    dsc.on_key_bad = [this, g] { on_key_bad(g); };
    dsc.on_packet = [this, g](const std::uint8_t* packet, bool) {
        if (current(g)) output_.feed(packet);   // S11
    };
    // REQ-49: the stored key is set before the TS filters are started (inside KeyManager::start).
    if (!keys_.start(s_->number, g, s_->service_id, stream, dsc)) {
        fail(kReceiverError, "the TS filters cannot be opened");   // issue #24
    }
}

// ---------------------------------------------------------------- T-14, T-15, T-19, T-16, T-17, T-18

void SessionController::on_video1(std::uint64_t g, al::Protection protection) {
    if (!current(g) || !in({State::Starting, State::WaitingStream})) return;
    if (protection != al::Protection::Clear) {
        s_->protection = Decision::Protected;
        s_->key_set = protection == al::Protection::ProtectedKey;
        if (s_->state == State::Starting) {
            // Rule 3: only recorded, applied by T-06. Without a key the decisions are made, so the
            // stream deadline is cancelled; with a key it keeps running until the key decision.
            if (!s_->key_set) cancel_timer(Timer::Deadline);
            return;
        }
        s_->is_protected = true;
        if (!s_->key_set) {
            release();   // T-14 (REQ-45): no segment of the session is written
            set_state(State::KeyRequired, kNoKey);
            return;
        }
        set_state(State::ValidatingKey);   // T-15: AL-DSC checks the PES starts (S10)
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
    if (s_->state == State::ValidatingKey && s_->key_result == KeyResult::Accepted) {
        s_->playing_since = clock_.unix_time_s();   // T-18 (S12)
        set_state(State::Playing);
        return;
    }
    not_handled("EV-SEG1");
}

void SessionController::on_key_ok(std::uint64_t g) {
    if (!current(g) || !in({State::Starting, State::ValidatingKey})) return;
    // T-16 (in starting the same data-path actions, rule 3): held segments are written (REQ-52).
    cancel_timer(Timer::Deadline);
    s_->key_result = KeyResult::Accepted;
    output_.set_write_enabled(true);
}

void SessionController::on_key_bad(std::uint64_t g) {
    if (!current(g)) return;
    if (s_->state == State::Starting) {
        // Rule 3: recorded; discarding the key and RELEASE follow at T-06. The decision is made, so a
        // late /start response cannot turn it into INVALID_STREAM.
        s_->key_result = KeyResult::Rejected;
        cancel_timer(Timer::Deadline);
        return;
    }
    if (s_->state == State::ValidatingKey) reject_key();   // T-17
}

void SessionController::reject_key() {
    // T-17 (REQ-53): the key of the service is discarded; RELEASE includes clear_keys, the held output
    // is discarded and POST /stop is sent.
    s_->key_result = KeyResult::Rejected;
    keys_.discard(s_->service_id);
    release();
    set_state(State::KeyRequired, kKeyRejected);
}

// ---------------------------------------------------------------- timers, T-26, T-27

void SessionController::on_timer(std::uint64_t g, Timer timer) {
    if (!current(g)) return;
    timer_slot(timer).reset();
    if (timer == Timer::Stats) {
        log_stats();
        start_timer(Timer::Stats, kStatsIntervalS);
        return;
    }
    if (timer == Timer::Keepalive &&
        in({State::WaitingStream, State::ValidatingKey, State::Playing, State::NoSignal})) {
        // T-26: POST /keepalive; restart the keepalive timer.
        start_timer(Timer::Keepalive, config_.keepalive_interval_s);
        keepalive_token_ = headend_.keepalive(s_->stream_control_url, s_->headend_session_id,
                                              [this, g](const HeadendError& e) { on_keepalive(g, e); });
        return;
    }
    if (timer == Timer::NoData && s_->state == State::WaitingStream) {
        set_state(State::NoSignal, kNoDataAtStart);   // T-20: the session and the keepalives continue
        return;
    }
    if (timer == Timer::Deadline && in({State::Starting, State::WaitingStream, State::ValidatingKey})) {
        // T-21: no protection or key decision 3 s after the first Locked; the stored key is kept.
        fail(kInvalidStream, "no protection or key decision within " + std::to_string(config_.stream_deadline_s) +
                                 " s after Locked");
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
    if (error.code == kSessionNotFound) {
        // T-28: the head-end does not know the session any more; no /stop for it.
        s_->headend_session_id.clear();
        fail(kHeadendUnreachable, "keepalive answered SESSION_NOT_FOUND");
        return;
    }
    // T-29: three consecutive failures (keepalive_loss_s / keepalive_interval_s, REQ-69).
    const int limit = std::max(1, config_.keepalive_loss_s / std::max(1, config_.keepalive_interval_s));
    ++s_->keepalive_failures;
    log(pal::LogLevel::Warn, "keepalive failed (" + std::to_string(s_->keepalive_failures) + " of " +
                                 std::to_string(limit) + "): " + error.code);
    if (s_->keepalive_failures >= limit) {
        fail(kHeadendUnreachable, std::to_string(limit) + " consecutive keepalives failed");
    }
}

// ---------------------------------------------------------------- RELEASE (rule 4)

bool SessionController::release(HeadendClient::Done on_stopped) {
    // 1. Timers and pending HTTP requests of the attempt.
    for (Timer t : {Timer::Keepalive, Timer::NoData, Timer::Deadline, Timer::Stats}) cancel_timer(t);
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
        headend_.stop(s_->stream_control_url, s_->headend_session_id, std::move(on_stopped));
        s_->headend_session_id.clear();
        return true;
    }
    return false;
}

void SessionController::log_stats() {
    // Section 4.8.1, principle 4: the counters of the tuner, the demux, the task queue (B4), AL-PSI and
    // AL-OUT (B5). Format of HANDOVER §5, extended by the discarded sections and segments.
    const pal::ITuner::Stats t = tuner_.stats();
    const pal::IDemux::Stats d = demux_.stats();
    log(pal::LogLevel::Info, "stats rtp_packets=" + std::to_string(t.rtp_packets) +
                                 " rtp_lost=" + std::to_string(t.rtp_lost) +
                                 " rtp_invalid=" + std::to_string(t.rtp_invalid) +
                                 " ts_packets=" + std::to_string(d.ts_packets) +
                                 " sync_errors=" + std::to_string(d.sync_errors) +
                                 " cc_errors=" + std::to_string(d.cc_errors) +
                                 " dropped_tasks=" + std::to_string(queue_.dropped_data_tasks()) +
                                 " discarded_sections=" + std::to_string(psi_.discarded_sections()) +
                                 " discarded_segments=" + std::to_string(output_.discarded_segments()));
}

void SessionController::fail(const std::string& code, const std::string& message) {
    release();
    s_->error = HeadendError{code, message};
    log(pal::LogLevel::Warn, "error " + code + ": " + message);
    set_state(State::Error);
}

// ---------------------------------------------------------------- helpers

std::optional<pal::IClock::TimerId>& SessionController::timer_slot(Timer timer) {
    switch (timer) {
        case Timer::Keepalive: return keepalive_timer_;
        case Timer::NoData: return no_data_timer_;
        case Timer::Stats: return stats_timer_;
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
    if (state != State::Error) s_->error.reset();   // Section 3.6.3: the error object only in state error
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
