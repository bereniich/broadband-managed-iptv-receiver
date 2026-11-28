// AL-SES module tests (step 7c, clear path): TST-131, TST-132 (a), TST-135, TST-155 and the rest of
// TST-105 / TST-106; step 9a: TST-133, TST-134, TST-138 and TST-127 at session level. All AL modules are real; the platform is the fakes of step 3, wired as AL-MAIN
// wires the real one. Expected values: the transition table of Section 4.3.4, the steps of
// Section 4.2.4, the JSON examples of Section 3 and the clamping rule of REQ-41 / Section 5.2.4.
#include <gtest/gtest.h>

#include "session_harness.hpp"

namespace {

using namespace sestest;
using fakes::FakeHttpClient;
using nlohmann::json;
using testutil::Bytes;

// ---------------------------------------------------------------- TST-131

TEST(Tst131, ClearLiveSynthetic) {
    Harness h;
    const std::uint64_t n = h.ses.play("svc-1", {});
    EXPECT_EQ(n, 1u);
    EXPECT_TRUE(h.states.empty());   // the response to play is sent before the first event of the session
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream"}));

    // T-03 / S3 before S4: untune, tune({5004, ssrc}), open_session(1), PAT filter, POST /start.
    const std::vector<std::string> e = h.calls.entries();
    const std::string tune = "tune 5004 " + std::to_string(h.ses.ssrc());
    const std::size_t i_info = index_of(e, "GET " + kMw + "/services/svc-1");
    const std::size_t i_untune = index_of(e, "untune", i_info);
    const std::size_t i_tune = index_of(e, tune, i_untune);
    const std::size_t i_open = index_of(e, "open_session 1", i_tune);
    const std::size_t i_pat = index_of(e, "open_filter section pid=0 table_id=0", i_open);
    const std::size_t i_start = index_of(e, "POST " + kHe + "/start", i_pat);
    EXPECT_LT(i_start, e.size()) << "order: GET info, untune, tune, open_session, PAT filter, POST /start";
    EXPECT_EQ(h.last_start_body()["ssrc"].get<std::uint32_t>(), h.ses.ssrc());
    EXPECT_EQ(h.last_start_body()["start"], "live");

    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(h.gen.frames(51));   // first segment listed at the key frame of 1 s
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "playing"}));
    const al::SessionState playing = h.states.back();
    EXPECT_EQ(playing.is_protected, std::optional<bool>(false));
    EXPECT_EQ(playing.start_time, std::optional<std::int64_t>(1794999600));   // he_start.json
    EXPECT_EQ(playing.playing_since, std::optional<std::int64_t>(kT));
    EXPECT_TRUE(playing.start.live());
    EXPECT_TRUE(h.store.session_exists(1));

    // T-26 / T-27: a keepalive every 5 s after EV-START-OK, answered 200.
    EXPECT_EQ(h.http.count("POST", kHe + "/keepalive"), 0u);
    h.advance(5000);
    EXPECT_EQ(h.http.count("POST", kHe + "/keepalive"), 1u);
    h.advance(5000);
    EXPECT_EQ(h.http.count("POST", kHe + "/keepalive"), 2u);
    EXPECT_EQ(json::parse(h.http.last("POST", kHe + "/keepalive")->body)["session_id"], "hs-000017");
    EXPECT_EQ(h.ses.state().state, al::State::Playing);
    EXPECT_TRUE(h.logger.contains("tuner Locked session=1 generation=1"));
    EXPECT_TRUE(h.logger.contains("play service=svc-1 start=live session=1 generation=1"));
}

TEST(Tst131, PlayReplacesSession) {
    Harness h;
    h.play_until_playing("svc-1");
    ASSERT_EQ(h.ses.state().state, al::State::Playing);
    const std::uint32_t old_ssrc = h.ses.ssrc();

    // Tasks of generation 1 still in the queue: a TS packet and a tuner status.
    h.demux.feed(h.gen.frames(1)[0]);
    h.tuner.emit(pal::TunerStatus::Unlocked);
    h.calls.clear();

    const std::uint64_t n = h.ses.play("svc-2", {});
    EXPECT_EQ(n, 2u);
    // RELEASE of session 1 (rule 4): filters closed, clear_keys, untune, close_session, POST /stop.
    const std::vector<std::string> e = h.calls.entries();
    const std::vector<std::string> expected = {"close_filter 1", "close_filter 2", "close_filter 3", "close_filter 4",
                                               "close_filter 5", "close_filter 6", "clear_keys", "untune",
                                               "close_session 1", "POST " + kHe + "/stop"};
    ASSERT_GE(e.size(), expected.size());
    EXPECT_EQ(std::vector<std::string>(e.begin(), e.begin() + static_cast<std::ptrdiff_t>(expected.size())), expected);
    EXPECT_EQ(json::parse(h.http.last("POST", kHe + "/stop")->body)["session_id"], "hs-000017");
    ASSERT_FALSE(h.states.empty());
    EXPECT_EQ(h.states.back().session, 1u);   // stopped of session 1, before play returned
    EXPECT_EQ(h.states.back().state, al::State::Stopped);
    const std::size_t events_before = h.states.size();

    h.settle();
    // Session 2 starts with a new SSRC; the old tasks changed nothing and produced no event.
    EXPECT_NE(h.ses.ssrc(), old_ssrc);
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "playing", "stopped"}));
    EXPECT_EQ(h.state_names(2), (std::vector<std::string>{"starting", "waiting_stream"}));
    EXPECT_EQ(h.states.size(), events_before + 2);
    EXPECT_NE(index_of(h.calls.entries(), "tune 5004 " + std::to_string(h.ses.ssrc())), h.calls.entries().size());
    EXPECT_TRUE(h.logger.contains("discarded tuner status of generation 1"));
    EXPECT_FALSE(h.store.session_exists(1));
}

TEST(Tst131, StopReleases) {
    Harness h;
    h.play_until_playing();
    h.calls.clear();
    h.ses.stop();   // T-02
    EXPECT_EQ(h.states.back().state, al::State::Stopped);
    EXPECT_NE(index_of(h.calls.entries(), "clear_keys"), h.calls.entries().size());
    EXPECT_NE(index_of(h.calls.entries(), "POST " + kHe + "/stop"), h.calls.entries().size());
    const std::size_t events = h.states.size();
    h.ses.stop();   // nothing left in stopped
    h.advance(20000);
    EXPECT_EQ(h.states.size(), events);
    EXPECT_EQ(h.http.count("POST", kHe + "/keepalive"), 0u);   // keepalive timer cancelled by RELEASE
}

TEST(Tst131, ClearLiveContent) {
    Harness h;
    h.ses.play("svc-1", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    // 5 s: with the PCR of FFmpeg content, the first segment is often 2 GOPs long (see the report of
    // step 7c), so the first key frame plus 2 s of content are needed in the worst case.
    if (!h.feed_content("svc-1.ts", 5.0)) return;
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "playing"}));
}

// ---------------------------------------------------------------- TST-132 (a)

TEST(Tst132, DataBeforeStartResponse) {
    Harness h;
    h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json").held());
    h.ses.play("svc-1", {});
    h.settle();
    ASSERT_EQ(h.state_names(1), std::vector<std::string>{"starting"});
    // The head-end sends before its response is processed: Locked, PSI, video, first segment.
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(h.gen.frames(51));
    EXPECT_EQ(h.state_names(1), std::vector<std::string>{"starting"});   // no state sent before the response
    EXPECT_TRUE(h.store.session_exists(1));
    h.advance(5000);   // 5 s after Locked: the stream deadline (3 s) was cancelled at EV-VIDEO1
    EXPECT_FALSE(h.logger.contains("EV-T-DEADLINE"));

    ASSERT_TRUE(h.http.release(h.http.last("POST", kHe + "/start")->id));
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "playing"}));   // directly playing
    EXPECT_EQ(h.states.back().is_protected, std::optional<bool>(false));
    EXPECT_EQ(h.states.back().start_time, std::optional<std::int64_t>(1794999600));
}

// ---------------------------------------------------------------- TST-135

TEST(Tst135, Clamping) {
    Harness h;   // clock at t = 1795000000
    const struct {
        std::optional<std::int64_t> requested;
        std::optional<std::int64_t> expected;   // nullopt = "live"
    } cases[] = {{kT - 8000, kT - 7195}, {kT - 7195, kT - 7195}, {kT - 60, kT - 60}, {kT, std::nullopt},
                 {kT + 100, std::nullopt}};
    for (const auto& c : cases) {
        h.ses.play("svc-1", al::StartPoint{c.requested});
        h.settle();
        EXPECT_EQ(h.ses.state().start.time, c.expected);
        const json start = h.last_start_body()["start"];
        if (c.expected) {
            EXPECT_EQ(start, *c.expected);
        } else {
            EXPECT_EQ(start, "live");
        }
        EXPECT_EQ(h.states.back().start.time, c.expected);
    }
}

// ---------------------------------------------------------------- TST-155

TEST(Tst155, ServiceInfoOnceForTwoPlays) {
    Harness h;
    h.ses.play("svc-1", {});
    h.settle();
    h.ses.play("svc-1", {});
    h.settle();
    EXPECT_EQ(h.http.count("GET", kMw + "/services/svc-1"), 1u);
    EXPECT_EQ(h.http.count("POST", kHe + "/start"), 2u);
}

// ---------------------------------------------------------------- TST-105, TST-106 (complete)

std::vector<std::string> filter_opens(const std::vector<std::string>& entries) {
    std::vector<std::string> opens;
    for (const std::string& e : entries) {
        if (e.rfind("open_filter", 0) == 0) opens.push_back(e.substr(0, e.find(" ->")));
    }
    return opens;
}

TEST(Tst105, FullFilterOrder) {
    Harness h;
    h.play_until_playing();
    const std::vector<std::string> expected = {"open_filter section pid=0 table_id=0",
                                               "open_filter section pid=4096 table_id=2", "open_filter ts pid=0",
                                               "open_filter ts pid=4096", "open_filter ts pid=256",
                                               "open_filter ts pid=257"};
    EXPECT_EQ(filter_opens(h.calls.entries()), expected);
    h.feed(h.gen.frames(50));   // PAT / PMT repetitions: no further filter call
    EXPECT_EQ(filter_opens(h.calls.entries()), expected);
}

TEST(Tst106, StreamInfoOncePerSession) {
    Harness h;
    h.play_until_playing();
    h.feed(h.gen.frames(50));
    ASSERT_EQ(h.infos.size(), 1u);
    const al::StreamDescription& d = h.infos[0].second;
    EXPECT_EQ(h.infos[0].first.session, 1u);
    EXPECT_EQ(d.program_number, 1);
    EXPECT_EQ(d.pmt_pid, 4096);
    EXPECT_EQ(d.pcr_pid, 256);
    EXPECT_EQ(d.video_pid, 256);
    EXPECT_EQ(d.video_stream_type, 0x1B);
    EXPECT_EQ(d.audio_pid, std::optional<std::uint16_t>(257));
    EXPECT_EQ(d.audio_stream_type, std::optional<std::uint8_t>(0x0F));
}

struct ContentCase {
    const char* file;
    std::uint16_t program, pmt_pid, video, audio;
};

TEST(Tst105, FullFilterOrderContent) {
    // Values of Section 5.2.1 (checked with ffprobe by TST-301).
    for (const ContentCase& c : {ContentCase{"svc-1.ts", 1, 4096, 256, 257},
                                 ContentCase{"test/svc-2.altpid.ts", 2, 768, 1025, 1026}}) {
        SCOPED_TRACE(c.file);
        Harness h;
        h.ses.play("svc-1", {});
        h.settle();
        ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
        h.settle();
        if (!h.feed_content(c.file, 1.0)) continue;
        const std::string pmt = std::to_string(c.pmt_pid);
        EXPECT_EQ(filter_opens(h.calls.entries()),
                  (std::vector<std::string>{"open_filter section pid=0 table_id=0",
                                            "open_filter section pid=" + pmt + " table_id=2", "open_filter ts pid=0",
                                            "open_filter ts pid=" + pmt, "open_filter ts pid=" + std::to_string(c.video),
                                            "open_filter ts pid=" + std::to_string(c.audio)}));
        ASSERT_EQ(h.infos.size(), 1u);
        EXPECT_EQ(h.infos[0].second.program_number, c.program);
        EXPECT_EQ(h.infos[0].second.video_pid, c.video);
        EXPECT_EQ(h.infos[0].second.audio_pid, std::optional<std::uint16_t>(c.audio));
    }
}

// ---------------------------------------------------------------- step 9a: errors of a start

// The error code of the last session_state of `session`, or "" if it is not error.

// RELEASE of an attempt without a head-end session (rule 4): the tuning, the PAT filter and the HLS
// directory of S3 are released, and no POST /stop is sent.
void expect_released_without_stop(const Harness& h) {
    const std::vector<std::string> e = h.calls.entries();
    const std::size_t tuned = index_of(e, "tune 5004");
    ASSERT_LT(tuned, e.size());
    EXPECT_LT(index_of(e, "untune", tuned), e.size());
    EXPECT_LT(index_of(e, "close_session 1", tuned), e.size());
    EXPECT_TRUE(h.demux.open_filters().empty());
    EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 0u);
}

TEST(Tst133, FutureTimeIsRetriedAsLive) {   // (a), T-07, ER-11
    Harness h;
    h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(400, "error_future_time.json"));
    h.ses.play("svc-1", al::StartPoint{kT - 10});
    h.settle();
    ASSERT_EQ(h.http.count("POST", kHe + "/start"), 2u);
    std::vector<json> starts;
    for (const auto& r : h.http.requests()) {
        if (r.method == "POST" && r.url == kHe + "/start") starts.push_back(json::parse(r.body));
    }
    ASSERT_EQ(starts.size(), 2u);
    EXPECT_EQ(starts[0]["start"], kT - 10);
    EXPECT_EQ(starts[1]["start"], "live");
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream"}));
    EXPECT_TRUE(h.states.back().start.live());   // session_state.start = "live"
    EXPECT_TRUE(h.logger.contains("answered FUTURE_TIME, retry as live"));
}

TEST(Tst133, HeadendErrorsEndInError) {   // (b) – (d), T-08, ER-10, ER-12, ER-13
    const struct {
        const char* name;
        FakeHttpClient::Reply reply;
        const char* code;
    } cases[] = {
        {"OUT_OF_ARCHIVE", FakeHttpClient::Reply::file(400, "error_out_of_archive.json"), "OUT_OF_ARCHIVE"},
        {"INVALID_REQUEST", FakeHttpClient::Reply::file(400, "error_invalid_request.json"), "INVALID_REQUEST"},
        {"transport failure", FakeHttpClient::Reply::transport_failure(), "HEADEND_UNREACHABLE"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        Harness h;
        h.http.script("POST", kHe + "/start", c.reply);
        h.ses.play("svc-1", al::StartPoint{kT - 10});
        h.settle();
        EXPECT_EQ(h.http.count("POST", kHe + "/start"), 1u);
        EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "error"}));
        EXPECT_EQ(error_code(h, 1), c.code);
        expect_released_without_stop(h);
        EXPECT_TRUE(h.logger.contains(std::string("error ") + c.code));
    }
}

TEST(Tst134, TuneFailure) {   // (a), T-04, ER-26
    Harness h;
    h.tuner.set_tune_result(false);
    h.ses.play("svc-1", {});
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "error"}));
    EXPECT_EQ(error_code(h, 1), "RECEIVER_ERROR");
    EXPECT_EQ(h.http.count("POST", kHe + "/start"), 0u);
    EXPECT_EQ(h.calls.find("open_session"), fakes::CallLog::npos);
}

TEST(Tst134, ServiceInfoErrors) {   // (b), (c), T-05, ER-08
    const struct {
        const char* name;
        FakeHttpClient::Reply reply;
        const char* code;
    } cases[] = {
        {"404", FakeHttpClient::Reply::file(404, "error_service_not_found.json"), "SERVICE_NOT_FOUND"},
        {"fails", FakeHttpClient::Reply::transport_failure(), "MW_UNREACHABLE"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        Harness h;
        h.http.script("GET", kMw + "/services/svc-1", c.reply);
        h.ses.play("svc-1", {});
        h.settle();
        EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "error"}));
        EXPECT_EQ(error_code(h, 1), c.code);
        EXPECT_EQ(h.http.count("POST", kHe + "/start"), 0u);
        EXPECT_EQ(h.calls.find("tune "), fakes::CallLog::npos);
    }
}

TEST(Tst134, FilterCannotBeOpened) {   // issue #24 (proposed T-34): open_filter returns 0
    Harness h;
    h.demux.fail_open_filters(true);
    h.ses.play("svc-1", {});
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "error"}));
    EXPECT_EQ(error_code(h, 1), "RECEIVER_ERROR");
    EXPECT_EQ(h.http.count("POST", kHe + "/start"), 0u);
    expect_released_without_stop(h);
}

TEST(Tst138, PmtWithoutVideoContent) {   // (a), T-13, ER-25
    Harness h;
    h.ses.play("svc-1", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    if (!h.feed_content("test/novideo.ts", 0.5)) return;
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "error"}));
    EXPECT_EQ(error_code(h, 1), "INVALID_STREAM");
    EXPECT_TRUE(h.logger.contains("PMT pcr_pid=256 video=none audio=256/0x0F"));
    EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 1u);   // RELEASE with the head-end session
    EXPECT_TRUE(h.infos.empty());                         // no stream_info for a stream without video
}

TEST(Tst138, StreamDeadline) {   // (b), T-21, ER-25: PAT and PMT, but no video packet
    Harness h;
    ASSERT_TRUE(h.dsc.store("svc-3", "3A7F1C9E5B2D8046A1E3C5079B4D6F28"));   // K3 (Section 5.2.2)
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    std::vector<Bytes> psi_only;
    for (const Bytes& p : h.gen.frames(1)) {
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid == 0 || pid == 4096) psi_only.push_back(p);
    }
    ASSERT_EQ(psi_only.size(), 2u);
    h.feed(psi_only);
    EXPECT_NE(h.calls.find("set_key 256"), fakes::CallLog::npos);   // key set at T-12 (REQ-49)
    h.advance(2999);
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream"}));
    h.advance(1);   // 3 000 ms after Locked
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "error"}));
    EXPECT_EQ(error_code(h, 1), "INVALID_STREAM");
    EXPECT_TRUE(h.dsc.has_key("svc-3"));   // the stored key is kept
    EXPECT_NE(h.calls.find("clear_keys"), fakes::CallLog::npos);
}

TEST(Tst127, WriteFailureAtSessionLevel) {   // T-30, ER-26
    Harness h;
    h.store.fail_writes_from(3);   // seg-0.ts and index.m3u8 succeed, seg-1.ts fails
    h.play_until_playing();
    ASSERT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "playing"}));
    h.feed(h.gen.frames(26));
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "playing", "error"}));
    EXPECT_EQ(error_code(h, 1), "RECEIVER_ERROR");
    // RELEASE: filters closed, clear_keys, untune, close_session, POST /stop.
    const std::vector<std::string> e = h.calls.entries();
    const std::size_t failed = index_of(e, "write_file 1 seg-1.ts");
    ASSERT_LT(failed, e.size());
    const std::size_t clear = index_of(e, "clear_keys", failed);
    const std::size_t untune = index_of(e, "untune", clear);
    const std::size_t closed = index_of(e, "close_session 1", untune);
    EXPECT_LT(clear, untune);
    EXPECT_LT(untune, closed);
    EXPECT_LT(closed, e.size());
    EXPECT_TRUE(h.demux.open_filters().empty());
    EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 1u);
}

TEST(Tst134, PmtAndTsFiltersCannotBeOpened) {   // issue #24 at T-11 and T-12
    for (std::size_t failing : {2u, 3u}) {   // open_filter calls: 1 PAT, 2 PMT, 3 the first TS filter
        SCOPED_TRACE(failing == 2 ? "PMT filter" : "TS filter");
        Harness h;
        h.ses.play("svc-1", {});
        h.settle();   // T-03 opened the PAT filter, /start answered
        h.demux.fail_open_filters_from(failing - 1);
        ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
        h.settle();
        h.feed(h.gen.frames(1));   // PAT, PMT
        EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "error"}));
        EXPECT_EQ(error_code(h, 1), "RECEIVER_ERROR");
        EXPECT_TRUE(h.demux.open_filters().empty());
        EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 1u);
    }
}

// ---------------------------------------------------------------- step 9b: no signal, keepalive

TEST(Tst136, NoDataAtStart) {   // T-20, T-32, T-10, ER-19
    Harness h;
    h.ses.play("svc-1", {});
    h.settle();   // /start answered 200: waiting_stream, no tuner status
    ASSERT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream"}));
    h.advance(4999);
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream"}));
    h.advance(1);   // 5 000 ms after EV-START-OK
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "no_signal"}));
    EXPECT_EQ(h.states.back().reason, std::optional<std::string>("no_data_at_start"));
    EXPECT_EQ(h.http.count("POST", kHe + "/keepalive"), 1u);   // the session and its keepalives continue
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Unlocked));
    h.settle();
    EXPECT_EQ(h.states.size(), 3u);   // T-32: no change
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    EXPECT_EQ(h.state_names(1),
              (std::vector<std::string>{"starting", "waiting_stream", "no_signal", "waiting_stream"}));
    EXPECT_FALSE(h.states.back().reason.has_value());
    h.advance(3000);   // T-10 is "as T-09": the stream deadline runs from this Locked
    EXPECT_EQ(error_code(h, 1), "INVALID_STREAM");
}

TEST(Tst137, DataLostWhilePlaying) {   // T-22, T-23, ER-20
    Harness h;
    h.play_until_playing();
    const std::size_t files = h.store.files(1).size();
    ASSERT_GT(files, 0u);
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Unlocked));
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "playing", "no_signal"}));
    EXPECT_EQ(h.states.back().reason, std::optional<std::string>("data_lost"));
    EXPECT_EQ(h.store.files(1).size(), files);   // directory and files kept
    EXPECT_EQ(h.calls.find("close_session"), fakes::CallLog::npos);
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    EXPECT_EQ(h.state_names(1),
              (std::vector<std::string>{"starting", "waiting_stream", "playing", "no_signal", "playing"}));
    EXPECT_FALSE(h.states.back().reason.has_value());
    for (const auto& st : h.states) EXPECT_EQ(st.session, 1u);   // no new session number
    EXPECT_TRUE(h.states.back().playing_since.has_value());
}

TEST(Tst145, KeepalivesEvery5s) {   // (1) T-26, T-27
    Harness h;
    h.play_until_playing();   // EV-START-OK at t0 = the time of play
    for (int k = 1; k <= 6; ++k) {
        h.advance(4999);
        EXPECT_EQ(h.http.count("POST", kHe + "/keepalive"), static_cast<std::size_t>(k - 1)) << k * 5 << " s";
        h.advance(1);
        EXPECT_EQ(h.http.count("POST", kHe + "/keepalive"), static_cast<std::size_t>(k)) << k * 5 << " s";
    }
    EXPECT_EQ(h.states.back().state, al::State::Playing);
}

TEST(Tst145, SessionNotFound) {   // (2) T-28, ER-14
    Harness h;
    h.http.script("POST", kHe + "/keepalive", FakeHttpClient::Reply::file(404, "error_session_not_found.json"));
    h.play_until_playing();
    h.advance(5000);
    EXPECT_EQ(error_code(h, 1), "HEADEND_UNREACHABLE");
    EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 0u);   // the head-end does not know the session
    EXPECT_TRUE(h.demux.open_filters().empty());
    EXPECT_NE(h.calls.find("close_session 1"), fakes::CallLog::npos);
}

TEST(Tst145, ThreeFailedKeepalives) {   // (3) T-29, ER-15
    Harness h;
    for (int i = 0; i < 3; ++i) h.http.script("POST", kHe + "/keepalive", FakeHttpClient::Reply::transport_failure());
    h.play_until_playing();
    h.advance(5000);
    h.advance(5000);
    EXPECT_EQ(h.states.back().state, al::State::Playing);   // no change at the failures of 5 and 10 s
    h.advance(4999);
    EXPECT_EQ(h.states.back().state, al::State::Playing);
    h.advance(1);   // the third failure, 15 s
    EXPECT_EQ(error_code(h, 1), "HEADEND_UNREACHABLE");
    EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 1u);   // RELEASE; the response is only logged
    EXPECT_EQ(h.logger.count_containing("keepalive failed ("), 3u);   // AL-SES: 1, 2 and 3 of 3
}

TEST(Tst145, SuccessResetsTheCount) {   // (4) T-27
    Harness h;
    using R = FakeHttpClient::Reply;
    for (const R& r : {R::transport_failure(), R::transport_failure(), R::ok(200, R"({ "expires_in_s": 15 })"),
                       R::transport_failure(), R::transport_failure()}) {
        h.http.script("POST", kHe + "/keepalive", r);
    }
    h.play_until_playing();
    for (int k = 0; k < 5; ++k) h.advance(5000);   // keepalives at 5, 10, 15, 20 and 25 s
    EXPECT_EQ(h.http.count("POST", kHe + "/keepalive"), 5u);
    EXPECT_EQ(h.states.back().state, al::State::Playing);
    EXPECT_EQ(error_code(h, 1), "");
}

// ---------------------------------------------------------------- step 9c: statistics every 10 s

std::vector<std::string> stats_lines(const Harness& h) {
    std::vector<std::string> out;
    for (const auto& l : h.logger.lines()) {
        if (std::string(l.module) == "AL-SES" && l.message.rfind("stats ", 0) == 0) out.push_back(l.message);
    }
    return out;
}

TEST(Tst148, StatsLineEvery10s) {   // Section 4.8.1, principle 4; HANDOVER §5 format
    Harness h;
    h.tuner.set_stats(pal::ITuner::Stats{1000, 2, 3});
    h.ses.play("svc-1", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    Bytes bad_pat = testutil::make_pat({{1, 4096}});
    bad_pat[bad_pat.size() - 1] ^= 0x01;   // CRC error: discarded by AL-PSI
    h.feed(testutil::packetize_section(0, bad_pat, 0));
    h.feed(h.gen.frames(51));   // playing
    h.advance(9999);
    EXPECT_TRUE(stats_lines(h).empty());
    h.advance(1);   // 10 s after T-03
    const pal::IDemux::Stats d = h.demux.stats();   // the bad PAT also repeats a continuity counter
    ASSERT_EQ(stats_lines(h).size(), 1u);
    EXPECT_EQ(stats_lines(h)[0], "stats rtp_packets=1000 rtp_lost=2 rtp_invalid=3 ts_packets=" +
                                     std::to_string(d.ts_packets) + " sync_errors=" + std::to_string(d.sync_errors) +
                                     " cc_errors=" + std::to_string(d.cc_errors) +
                                     " dropped_tasks=0 discarded_sections=1 discarded_segments=0 session=1 generation=1");
    EXPECT_GT(d.ts_packets, 0u);
    h.advance(10000);
    h.advance(10000);
    EXPECT_EQ(stats_lines(h).size(), 3u);   // at 10, 20 and 30 s
    h.ses.stop();   // RELEASE cancels the statistics timer
    h.settle();
    h.advance(30000);
    EXPECT_EQ(stats_lines(h).size(), 3u);
}

}  // namespace
