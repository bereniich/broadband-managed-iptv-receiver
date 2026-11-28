// AL-SES module tests of the protected stream (step 11a): TST-139, TST-132 (b) and the T-06 variants of
// rule 3, the session-level parts of TST-113, TST-114 and TST-116, and TST-138 for the stream deadline
// in validating_key (T-21). Expected values: the transition table of Section 4.3.4 (T-06, T-14 –
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

}  // namespace
