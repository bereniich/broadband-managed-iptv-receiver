// AL-SES module tests of the protected stream (step 11a): TST-139, TST-132 (b) and the T-06 variants of
// rule 3, the session-level parts of TST-113, TST-114 and TST-116, and TST-138 for the stream deadline
// in validating_key (T-21); step 11b, the restarts by set_key and the concurrency cases: TST-140 –
// TST-144 and TST-147 (T-24, T-25, T-33 with the guard of specification 1.3, T-01, T-02). Expected values: the transition table of Section 4.3.4 (T-06, T-14 –
// T-18, T-21, rule 3 and RELEASE of Section 4.3.1), the test rows of Section 5.3.2, the keys of
// Section 5.2.2 and the JSON examples of Section 3. The synthetic streams are encrypted by the
// reference implementation of D-04 of the test code (StreamGen::key), not by HAL-DSC or package_ts.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>

#include "session_harness.hpp"

namespace {

using namespace sestest;
using fakes::FakeHttpClient;
using nlohmann::json;
using testutil::Bytes;

// Keys of Section 5.2.2.
const std::string kK3 = "3A7F1C9E5B2D8046A1E3C5079B4D6F28";
const std::string kK4 = "C04E92B7165FD83A2E7B0C9145D6A3F1";
const std::string kKX = "00112233445566778899AABBCCDDEEFF";
const fakes::AesKey kK3Bytes = {0x3A, 0x7F, 0x1C, 0x9E, 0x5B, 0x2D, 0x80, 0x46,
                                0xA1, 0xE3, 0xC5, 0x07, 0x9B, 0x4D, 0x6F, 0x28};
constexpr std::int64_t kArchiveT = kT - 600;   // TST-139: play svc-3 with T = t - 600

// The packets of `packets` whose PID is one of `pids`.
std::vector<Bytes> only_pid(const std::vector<Bytes>& packets, std::initializer_list<int> pids) {
    std::vector<Bytes> out;
    for (const Bytes& p : packets) {
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        for (int want : pids) {
            if (pid == want) out.push_back(p);
        }
    }
    return out;
}

const al::SessionState& last_of(const Harness& h, std::uint64_t session) {
    for (auto it = h.states.rbegin(); it != h.states.rend(); ++it) {
        if (it->session == session) return *it;
    }
    static const al::SessionState none;
    return none;
}

// ---------------------------------------------------------------- TST-139

TEST(Tst139, NoKeySynthetic) {   // T-14, REQ-45, RELEASE (rule 4)
    Harness h;
    h.gen.key = kK3Bytes;   // protected service, no key stored
    h.ses.play("svc-3", al::StartPoint{kArchiveT});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    const std::size_t from = h.calls.entries().size();
    h.feed(h.gen.frames(51));

    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "key_required"}));
    const al::SessionState& s = last_of(h, 1);
    EXPECT_EQ(s.reason, std::optional<std::string>("no_key"));
    EXPECT_EQ(s.start.time, std::optional<std::int64_t>(kArchiveT));   // start point kept (D-14)
    EXPECT_EQ(s.is_protected, std::optional<bool>(true));

    // RELEASE in the order of rule 4: filters closed, clear_keys, untune, close_session, POST /stop.
    const std::vector<std::string> e = h.calls.entries();
    const std::size_t i_close = index_of(e, "close_filter", from);
    const std::size_t i_clear = index_of(e, "clear_keys", i_close);
    const std::size_t i_untune = index_of(e, "untune", i_clear);
    const std::size_t i_session = index_of(e, "close_session 1", i_untune);
    const std::size_t i_stop = index_of(e, "POST " + kHe + "/stop", i_session);
    EXPECT_LT(i_stop, e.size()) << "order: close_filter, clear_keys, untune, close_session, POST /stop";
    EXPECT_EQ(json::parse(h.http.last("POST", kHe + "/stop")->body)["session_id"], "hs-000017");
    EXPECT_EQ(index_of(e, "write_file"), e.size()) << "REQ-45: no write_file during the whole session";
    // Timers and keepalives cancelled by RELEASE (rule 4, step 1).
    const std::size_t keepalives = h.http.count("POST", kHe + "/keepalive");
    h.advance(20000);
    EXPECT_EQ(h.http.count("POST", kHe + "/keepalive"), keepalives);
    EXPECT_EQ(h.state_names(1).size(), 3u);
}

TEST(Tst139, ContentSvc3NoKey) {
    Harness h;
    h.ses.play("svc-3", al::StartPoint{kArchiveT});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    if (!h.feed_content("svc-3.enc.ts", 2.0)) return;
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "key_required"}));
    EXPECT_EQ(last_of(h, 1).reason, std::optional<std::string>("no_key"));
    EXPECT_EQ(last_of(h, 1).start.time, std::optional<std::int64_t>(kArchiveT));
    EXPECT_EQ(index_of(h.calls.entries(), "write_file"), h.calls.entries().size());
    EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 1u);
}

// ---------------------------------------------------------------- TST-113, TST-114 (session level)

TEST(Tst113, SessionKeyAcceptedSynthetic) {   // T-15, T-16, T-18
    Harness h;
    h.gen.key = kK3Bytes;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(h.gen.frames(3));   // three frames: three PES starts received with scrambled = true
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key"}));
    EXPECT_EQ(last_of(h, 1).is_protected, std::optional<bool>(true));
    EXPECT_TRUE(h.logger.contains("key accepted service=svc-3 key=3A7F\xE2\x80\xA6"));
    h.advance(5000);   // the stream deadline (3 s) was cancelled at EV-KEY-OK (T-16)
    h.feed(h.gen.frames(51));
    EXPECT_EQ(h.state_names(1),
              (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "playing"}));
    EXPECT_EQ(last_of(h, 1).playing_since, std::optional<std::int64_t>(kT + 5));
    EXPECT_TRUE(h.dsc.has_key("svc-3"));
}

TEST(Tst113, ContentSvc3WithK3Session) {
    Harness h;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    if (!h.feed_content("svc-3.enc.ts", 5.0)) return;
    EXPECT_EQ(h.state_names(1),
              (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "playing"}));
    EXPECT_EQ(last_of(h, 1).is_protected, std::optional<bool>(true));
}

TEST(Tst114, ContentKxAndK4Session) {   // T-17, REQ-53, REQ-54, US-16 AC4
    for (const std::string& wrong : {kKX, kK4}) {
        SCOPED_TRACE(wrong);
        Harness h;
        ASSERT_TRUE(h.dsc.store("svc-3", wrong));
        h.ses.play("svc-3", {});
        h.settle();
        ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
        h.settle();
        const std::size_t from = h.calls.entries().size();
        if (!h.feed_content("svc-3.enc.ts", 2.0)) return;
        EXPECT_EQ(h.state_names(1),
                  (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "key_required"}));
        EXPECT_EQ(last_of(h, 1).reason, std::optional<std::string>("key_rejected"));
        EXPECT_FALSE(h.dsc.has_key("svc-3"));   // removed from the key store
        const std::vector<std::string> e = h.calls.entries();
        EXPECT_LT(index_of(e, "clear_keys", from), e.size());
        EXPECT_EQ(index_of(e, "write_file"), e.size());   // nothing passed to the player (D-11)
        EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 1u);
    }
}

// ---------------------------------------------------------------- TST-132 (b) and the other T-06 variants

TEST(Tst132, KeyRejectedBeforeStartResponse) {   // (b): rule 3, T-06, T-17
    Harness h;
    ASSERT_TRUE(h.dsc.store("svc-3", kKX));
    h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json").held());
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    if (!h.feed_content("svc-3.enc.ts", 1.0)) return;
    EXPECT_TRUE(h.logger.contains("key rejected service=svc-3"));   // EV-KEY-BAD before the response
    EXPECT_EQ(h.state_names(1), std::vector<std::string>{"starting"});
    EXPECT_TRUE(h.dsc.has_key("svc-3"));   // KX removed only at the response
    h.advance(5000);   // the deadline was cancelled at EV-KEY-BAD (rule 3)
    EXPECT_EQ(h.state_names(1), std::vector<std::string>{"starting"});

    ASSERT_TRUE(h.http.release(h.http.last("POST", kHe + "/start")->id));
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "key_required"}));
    EXPECT_EQ(last_of(h, 1).reason, std::optional<std::string>("key_rejected"));
    EXPECT_EQ(last_of(h, 1).is_protected, std::optional<bool>(true));
    EXPECT_FALSE(h.dsc.has_key("svc-3"));
    ASSERT_NE(h.http.last("POST", kHe + "/stop"), nullptr);   // RELEASE with the id of the response
    EXPECT_EQ(json::parse(h.http.last("POST", kHe + "/stop")->body)["session_id"], "hs-000017");
    EXPECT_EQ(index_of(h.calls.entries(), "write_file"), h.calls.entries().size());
}

TEST(Tst132, NoKeyBeforeStartResponse) {   // rule 3, T-06 as T-14
    Harness h;
    h.gen.key = kK3Bytes;
    h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json").held());
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(h.gen.frames(10));
    h.advance(5000);   // the deadline was cancelled at EV-VIDEO1 without a key (rule 3)
    EXPECT_EQ(h.state_names(1), std::vector<std::string>{"starting"});
    ASSERT_TRUE(h.http.release(h.http.last("POST", kHe + "/start")->id));
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "key_required"}));
    EXPECT_EQ(last_of(h, 1).reason, std::optional<std::string>("no_key"));
    EXPECT_EQ(last_of(h, 1).is_protected, std::optional<bool>(true));
    EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 1u);
}

TEST(Tst132, KeyAcceptedBeforeStartResponse) {   // rule 3, T-06 as T-15, T-16, T-18
    Harness h;
    h.gen.key = kK3Bytes;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json").held());
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(h.gen.frames(51));   // key accepted and the first segment listed before the response
    EXPECT_EQ(h.state_names(1), std::vector<std::string>{"starting"});
    const std::vector<std::string> files = h.store.files(1);
    EXPECT_NE(std::find(files.begin(), files.end(), "index.m3u8"), files.end());   // first segment listed
    ASSERT_TRUE(h.http.release(h.http.last("POST", kHe + "/start")->id));
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "playing"}));
    EXPECT_EQ(last_of(h, 1).is_protected, std::optional<bool>(true));
    EXPECT_EQ(last_of(h, 1).playing_since, std::optional<std::int64_t>(kT));
}

TEST(Tst132, KeyPendingAtStartResponse) {   // rule 3, T-06 as T-15: validating_key, then T-16, T-18
    Harness h;
    h.gen.key = kK3Bytes;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json").held());
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(only_pid(h.gen.frames(1), {0, 4096, 256}));   // PSI and one PES start: protection decided
    ASSERT_TRUE(h.http.release(h.http.last("POST", kHe + "/start")->id));
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "validating_key"}));
    h.feed(h.gen.frames(51));
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "validating_key", "playing"}));
}

// ---------------------------------------------------------------- TST-138: deadline in validating_key

TEST(Tst138, StreamDeadlineInValidatingKey) {   // T-21, ER-25
    Harness h;
    h.gen.key = kK3Bytes;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(only_pid(h.gen.frames(1), {0, 4096, 256}));   // one PES start of three, then nothing
    ASSERT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key"}));
    h.advance(2999);
    EXPECT_EQ(h.ses.state().state, al::State::ValidatingKey);
    h.advance(1);   // 3 000 ms after Locked
    EXPECT_EQ(h.state_names(1),
              (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "error"}));
    EXPECT_EQ(error_code(h, 1), "INVALID_STREAM");
    EXPECT_TRUE(h.dsc.has_key("svc-3"));   // T-21: the stored key is kept
}

// ---------------------------------------------------------------- TST-116 (session level)

TEST(Tst116, NoFullKeyInSessionLogs) {   // REQ-55
    Harness h;
    h.gen.key = kK3Bytes;
    ASSERT_TRUE(h.dsc.store("svc-3", kKX));   // rejected
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(h.gen.frames(3));
    ASSERT_EQ(h.ses.state().state, al::State::KeyRequired);
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));   // accepted
    ASSERT_TRUE(h.dsc.store("svc-4", kK4));
    h.ses.play("svc-3", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(h.gen.frames(51));
    ASSERT_EQ(h.ses.state().state, al::State::Playing);

    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    bool k3_masked = false, kx_masked = false, k4_masked = false;
    for (const auto& line : h.logger.lines()) {
        for (const std::string& key : {kK3, kK4, kKX}) {
            EXPECT_EQ(line.message.find(key), std::string::npos) << line.message;
            EXPECT_EQ(lower(line.message).find(lower(key)), std::string::npos) << line.message;
        }
        k3_masked |= line.message.find("key=3A7F\xE2\x80\xA6") != std::string::npos;
        kx_masked |= line.message.find("key=0011\xE2\x80\xA6") != std::string::npos;
        k4_masked |= line.message.find("key=C04E\xE2\x80\xA6") != std::string::npos;
    }
    EXPECT_TRUE(k3_masked);
    EXPECT_TRUE(kx_masked);
    EXPECT_TRUE(k4_masked);
}

// ---------------------------------------------------------------- step 11b helpers

// set_key as AL-API executes it (Section 4.2.4): AL-DSC stores the key, then EV-SETKEY to AL-SES.
void set_key(Harness& h, const std::string& service, const std::string& key) {
    ASSERT_TRUE(h.dsc.store(service, key));
    h.ses.set_key(service);
}

// After S2 – S4 of an attempt: Locked, then the given packets.
void lock_and_feed(Harness& h, const std::vector<Bytes>& packets) {
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    h.feed(packets);
}

// State after TST-139: a new session of svc-3 in key_required (no_key) with T = t - 600.
void to_key_required(Harness& h) {
    h.gen.key = kK3Bytes;
    const std::uint64_t n = h.ses.play("svc-3", al::StartPoint{kArchiveT});
    lock_and_feed(h, h.gen.frames(3));
    ASSERT_EQ(h.state_names(n), (std::vector<std::string>{"starting", "waiting_stream", "key_required"}));
}

std::vector<std::string> since(const Harness& h, std::size_t from, std::uint64_t session) {
    std::vector<std::string> names;
    for (std::size_t i = from; i < h.states.size(); ++i) {
        if (h.states[i].session == session) names.emplace_back(al::to_string(h.states[i].state));
    }
    return names;
}

// ---------------------------------------------------------------- TST-140

TEST(Tst140, CorrectKeyRestartsFromTheKeptStartPoint) {   // T-24, T-15, T-16, T-18; REQ-49, REQ-52, REQ-18
    Harness h;
    to_key_required(h);
    const std::uint32_t old_ssrc = h.ses.ssrc();
    const std::uint64_t old_generation = h.ses.generation();
    ASSERT_EQ(h.infos.size(), 1u);
    bool written_before_ok = false;
    h.store.set_write_hook([&](std::uint64_t, const std::string&) {
        if (!h.logger.contains("key accepted service=svc-3")) written_before_ok = true;
    });

    h.advance(120000);
    const std::size_t from = h.states.size();
    set_key(h, "svc-3", kK3);
    EXPECT_EQ(h.states.size(), from);   // starting follows as a task, after the response to set_key
    lock_and_feed(h, h.gen.frames(51));

    EXPECT_EQ(since(h, from, 1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "playing"}));
    for (std::size_t i = from; i < h.states.size(); ++i) EXPECT_EQ(h.states[i].session, 1u);   // same number
    EXPECT_NE(h.ses.ssrc(), old_ssrc);
    EXPECT_GT(h.ses.generation(), old_generation);
    EXPECT_EQ(h.last_start_body()["start"], kArchiveT);   // T, not T + 120 (D-14)
    EXPECT_EQ(h.last_start_body()["ssrc"].get<std::uint32_t>(), h.ses.ssrc());
    EXPECT_EQ(h.http.count("POST", kHe + "/start"), 2u);
    EXPECT_EQ(h.infos.size(), 1u);   // no second stream_info (REQ-18)
    EXPECT_FALSE(written_before_ok);
    EXPECT_FALSE(h.store.files(1).empty());
    EXPECT_EQ(h.states[from].is_protected, std::nullopt);   // protected null again after the restart
    EXPECT_EQ(last_of(h, 1).is_protected, std::optional<bool>(true));
    EXPECT_EQ(last_of(h, 1).start.time, std::optional<std::int64_t>(kArchiveT));
}

TEST(Tst140, ContentSvc3WithK3AfterKeyRequired) {
    Harness h;
    h.ses.play("svc-3", al::StartPoint{kArchiveT});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    if (!h.feed_content("svc-3.enc.ts", 1.0)) return;
    ASSERT_EQ(h.ses.state().state, al::State::KeyRequired);
    h.advance(120000);
    const std::size_t from = h.states.size();
    set_key(h, "svc-3", kK3);
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));
    h.settle();
    if (!h.feed_content("svc-3.enc.ts", 5.0)) return;
    EXPECT_EQ(since(h, from, 1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "playing"}));
    EXPECT_EQ(h.last_start_body()["start"], kArchiveT);
}

// ---------------------------------------------------------------- TST-141

TEST(Tst141, IncorrectKeyThenCorrectKey) {   // T-17, T-24; REQ-53; US-16 AC2, AC3
    Harness h;
    to_key_required(h);
    const std::size_t stops = h.http.count("POST", kHe + "/stop");

    std::size_t from = h.states.size();
    set_key(h, "svc-3", kKX);
    const std::size_t calls_from = h.calls.entries().size();
    lock_and_feed(h, h.gen.frames(3));
    EXPECT_EQ(since(h, from, 1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "key_required"}));
    EXPECT_EQ(last_of(h, 1).reason, std::optional<std::string>("key_rejected"));
    EXPECT_FALSE(h.dsc.has_key("svc-3"));   // KX removed
    EXPECT_LT(index_of(h.calls.entries(), "clear_keys", calls_from), h.calls.entries().size());
    EXPECT_EQ(h.out.held_segments(), 0u);   // held data discarded
    EXPECT_FALSE(h.store.session_exists(1));
    EXPECT_EQ(h.http.count("POST", kHe + "/stop"), stops + 1);

    h.advance(120000);
    from = h.states.size();
    set_key(h, "svc-3", kK3);
    lock_and_feed(h, h.gen.frames(51));
    EXPECT_EQ(since(h, from, 1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "playing"}));
    EXPECT_EQ(h.last_start_body()["start"], kArchiveT);
    EXPECT_TRUE(h.dsc.has_key("svc-3"));
}

// ---------------------------------------------------------------- TST-142

TEST(Tst142, SetKeyBeforePmtIsUsedAtT12) {   // (1) T-25, T-12
    Harness h;
    h.gen.key = kK3Bytes;
    ASSERT_TRUE(h.dsc.store("svc-3", kKX));
    h.ses.play("svc-3", {});
    const std::uint64_t g = h.ses.generation();
    set_key(h, "svc-3", kK3);   // starting, before EV-PMT
    lock_and_feed(h, h.gen.frames(51));
    EXPECT_EQ(h.ses.generation(), g);   // no restart
    EXPECT_EQ(h.http.count("POST", kHe + "/start"), 1u);
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "playing"}));
}

TEST(Tst142, SetKeyInValidatingKeyRestarts) {   // (2) T-33
    Harness h;
    h.gen.key = kK3Bytes;
    ASSERT_TRUE(h.dsc.store("svc-3", kKX));
    h.ses.play("svc-3", {});
    // PSI and a video continuation packet: protection decided (validating_key), no PES start checked yet.
    const std::vector<Bytes> frame = h.gen.frames(1);
    std::vector<Bytes> first = only_pid(frame, {0, 4096});
    for (const Bytes& p : only_pid(frame, {256})) {
        if ((p[1] & 0x40) == 0) {
            first.push_back(p);
            break;
        }
    }
    lock_and_feed(h, first);
    ASSERT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key"}));
    const std::uint64_t g = h.ses.generation();
    const std::uint32_t ssrc = h.ses.ssrc();
    const std::size_t from = h.states.size();
    set_key(h, "svc-3", kK3);
    lock_and_feed(h, h.gen.frames(51));
    EXPECT_EQ(since(h, from, 1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "playing"}));
    EXPECT_GT(h.ses.generation(), g);
    EXPECT_NE(h.ses.ssrc(), ssrc);
    EXPECT_TRUE(h.dsc.has_key("svc-3"));   // K3 stored, not removed
    EXPECT_FALSE(h.logger.contains("key rejected"));
    EXPECT_EQ(h.http.count("POST", kHe + "/stop"), 1u);   // RELEASE of the first attempt
}

TEST(Tst142, SetKeyForAnotherServiceChangesNothing) {   // (3) T-25
    Harness h;
    h.gen.key = kK3Bytes;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    h.ses.play("svc-3", {});
    lock_and_feed(h, h.gen.frames(51));
    ASSERT_EQ(h.ses.state().state, al::State::Playing);
    const std::uint64_t g = h.ses.generation();
    const std::size_t events = h.states.size();
    set_key(h, "svc-4", kK4);
    h.settle();
    EXPECT_TRUE(h.dsc.has_key("svc-4"));
    EXPECT_EQ(h.ses.generation(), g);
    EXPECT_EQ(h.states.size(), events);
}

TEST(Tst142, SetKeyAfterTheKeyWasAcceptedDoesNotRestart) {   // T-25 of specification 1.3 (issue #22)
    Harness h;
    h.gen.key = kK3Bytes;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    h.ses.play("svc-3", {});
    lock_and_feed(h, h.gen.frames(3));   // EV-KEY-OK; waiting for EV-SEG1 in validating_key
    ASSERT_EQ(h.ses.state().state, al::State::ValidatingKey);
    ASSERT_TRUE(h.logger.contains("key accepted service=svc-3"));
    const std::uint64_t g = h.ses.generation();
    set_key(h, "svc-3", kK3);
    h.feed(h.gen.frames(51));
    EXPECT_EQ(h.ses.generation(), g);
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "playing"}));
}

TEST(Tst142, SetKeyAfterPmtWithoutKeyRestarts) {   // T-33: PMT parsed without a key (REQ-49 of 1.3)
    Harness h;
    h.gen.key = kK3Bytes;
    h.ses.play("svc-3", {});
    lock_and_feed(h, only_pid(h.gen.frames(1), {0, 4096}));   // PMT parsed, protection still unknown
    ASSERT_EQ(h.ses.state().state, al::State::WaitingStream);
    const std::uint64_t g = h.ses.generation();
    const std::size_t from = h.states.size();
    set_key(h, "svc-3", kK3);
    lock_and_feed(h, h.gen.frames(51));
    EXPECT_GT(h.ses.generation(), g);
    EXPECT_EQ(since(h, from, 1), (std::vector<std::string>{"starting", "waiting_stream", "validating_key", "playing"}));
}

// ---------------------------------------------------------------- TST-143

TEST(Tst143, TasksOfTheOldGenerationChangeNothing) {   // T-01, rule 2; REQ-56; US-18
    Harness h;
    h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json").held());
    h.ses.play("svc-4", {});
    h.settle();
    ASSERT_TRUE(h.tuner.emit(pal::TunerStatus::Locked));   // starts the stream deadline of generation 1
    h.settle();
    const auto* old_start = h.http.last("POST", kHe + "/start");
    ASSERT_NE(old_start, nullptr);
    const std::uint64_t old_id = old_start->id;

    // Tasks of generation 1 posted but not yet executed: a filter-data task, a timer expiry and a
    // tuner status. The /start response is released after the new play.
    h.demux.feed(h.gen.frames(1)[0]);   // PAT on the open PAT filter
    h.clock.advance(3000);              // the stream deadline expires: its task is queued
    h.tuner.emit(pal::TunerStatus::Unlocked);
    h.calls.clear();
    h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json"));

    const std::uint64_t n = h.ses.play("svc-3", {});
    EXPECT_EQ(n, 2u);
    const std::vector<std::string> e = h.calls.entries();
    EXPECT_LT(index_of(e, "cancel " + std::to_string(old_id)), e.size());   // pending /start cancelled
    EXPECT_LT(index_of(e, "close_filter"), e.size());
    EXPECT_LT(index_of(e, "close_session 1"), e.size());
    EXPECT_FALSE(h.http.release(old_id));   // cancelled: never delivered (Section 3.9.1, rule 4)
    h.settle();

    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "stopped"}));
    EXPECT_EQ(h.state_names(2), (std::vector<std::string>{"starting", "waiting_stream"}));
    for (std::size_t i = 0; i < h.states.size(); ++i) {
        if (h.states[i].session == 1 && h.states[i].state == al::State::Stopped) {
            for (std::size_t j = i + 1; j < h.states.size(); ++j) EXPECT_EQ(h.states[j].session, 2u);
        }
    }
    EXPECT_TRUE(h.logger.contains("discarded tuner status of generation 1"));
    EXPECT_EQ(h.ses.state().state, al::State::WaitingStream);
    EXPECT_TRUE(h.infos.empty());
}

// ---------------------------------------------------------------- TST-144

TEST(Tst144, PlayInKeyRequiredAndError) {   // (1) T-01
    Harness h;
    to_key_required(h);
    h.ses.play("svc-1", {});
    h.settle();
    EXPECT_EQ(h.state_names(1), (std::vector<std::string>{"starting", "waiting_stream", "key_required", "stopped"}));
    EXPECT_EQ(h.state_names(2), (std::vector<std::string>{"starting", "waiting_stream"}));

    h.ses.play("svc-9", {});   // unknown service: GET /services/svc-9 answers 404 (T-05)
    h.settle();
    ASSERT_EQ(h.state_names(3), (std::vector<std::string>{"starting", "error"}));
    h.ses.play("svc-1", {});
    h.settle();
    EXPECT_EQ(h.state_names(3), (std::vector<std::string>{"starting", "error", "stopped"}));
    EXPECT_EQ(h.state_names(4), (std::vector<std::string>{"starting", "waiting_stream"}));
}

TEST(Tst144, StopInPlayingKeyRequiredAndError) {   // (2), (3) T-02
    Harness h;
    h.play_until_playing();
    h.calls.clear();
    h.ses.stop();
    EXPECT_EQ(h.state_names(1).back(), "stopped");
    EXPECT_LT(index_of(h.calls.entries(), "POST " + kHe + "/stop"), h.calls.entries().size());   // RELEASE

    for (int variant = 0; variant < 2; ++variant) {
        SCOPED_TRACE(variant == 0 ? "key_required" : "error");
        Harness k;
        if (variant == 0) {
            to_key_required(k);
        } else {
            k.ses.play("svc-9", {});
            k.settle();
            ASSERT_EQ(k.ses.state().state, al::State::Error);
        }
        k.calls.clear();
        k.ses.stop();
        EXPECT_EQ(k.ses.state().state, al::State::Stopped);
        EXPECT_EQ(k.states.back().state, al::State::Stopped);
        std::string calls;
        for (const std::string& c : k.calls.entries()) calls += c + "; ";
        EXPECT_TRUE(k.calls.entries().empty()) << "no PAL call: nothing left to release; calls: " << calls;
    }
}

// ---------------------------------------------------------------- TST-147

TEST(Tst147, LogOfEverySession) {   // REQ-59 for the sessions of TST-131 and TST-139 – TST-141
    Harness h;
    h.play_until_playing("svc-1");   // TST-131
    to_key_required(h);              // TST-139: session 2
    set_key(h, "svc-3", kKX);        // TST-141
    lock_and_feed(h, h.gen.frames(3));
    set_key(h, "svc-3", kK3);        // TST-140
    lock_and_feed(h, h.gen.frames(51));
    ASSERT_EQ(h.ses.state().state, al::State::Playing);

    auto has = [&](const std::string& module, const std::string& text) {
        for (const auto& line : h.logger.lines()) {
            if (line.module == module && line.message.find(text) != std::string::npos) return true;
        }
        return false;
    };
    // play (service, start)
    EXPECT_TRUE(has("AL-SES", "play service=svc-1 start=live session=1"));
    EXPECT_TRUE(has("AL-SES", "play service=svc-3 start=" + std::to_string(kArchiveT) + " session=2"));
    // head-end session (id, SSRC, start time)
    EXPECT_TRUE(has("AL-HEC", "start ok session=1 headend_session=hs-000017 ssrc="));
    EXPECT_TRUE(has("AL-HEC", "start ok session=2 headend_session=hs-000017 ssrc="));
    EXPECT_TRUE(has("AL-HEC", "start_time=1794999600"));
    // PAT / PMT result
    EXPECT_TRUE(has("AL-PSI", "PAT program=1 pmt_pid=4096"));
    EXPECT_TRUE(has("AL-PSI", "PMT pcr_pid=256 video=256/0x1B audio=257/0x0F"));
    // protection decision, key accepted / rejected
    EXPECT_TRUE(has("AL-DSC", "protection clear session=1"));
    EXPECT_TRUE(has("AL-DSC", "protection protected key_set=false session=2"));
    EXPECT_TRUE(has("AL-DSC", "protection protected key_set=true session=2"));
    EXPECT_TRUE(has("AL-DSC", "key rejected service=svc-3 key=0011\xE2\x80\xA6 session=2"));
    EXPECT_TRUE(has("AL-DSC", "key accepted service=svc-3 key=3A7F\xE2\x80\xA6 session=2"));
    // every state change (Section 3.6.3 values) of both sessions
    for (const char* change : {"state starting -> waiting_stream session=1", "state waiting_stream -> playing session=1",
                               "state playing -> stopped session=1", "state waiting_stream -> key_required session=2",
                               "state waiting_stream -> validating_key session=2",
                               "state validating_key -> key_required session=2",
                               "state validating_key -> playing session=2"}) {
        EXPECT_TRUE(has("AL-SES", change)) << change;
    }
    EXPECT_GE(h.logger.count_containing("state starting session=2"), 3u);   // the play and two restarts
}

}  // namespace
