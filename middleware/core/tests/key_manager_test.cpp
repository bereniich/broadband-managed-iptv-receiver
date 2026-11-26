// AL-DSC module tests (step 5): protection decision (TST-111), key validation (TST-113, TST-114),
// keys in logs (TST-116) and the TS filters of the output (TST-105, REQ-19, REQ-49). Protected packets
// without content files are made with the reference implementation of D-04 (tests/fakes/), checked
// with the FIPS-197 and SP 800-38A vectors in step 3a. The state changes of T-14 – T-19 are AL-SES.
#include <gtest/gtest.h>

#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "content.hpp"
#include "fakes/call_log.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_demux.hpp"
#include "fakes/fake_logger.hpp"
#include "fakes/reference_d04.hpp"
#include "key_manager.hpp"
#include "task_queue.hpp"
#include "ts_builder.hpp"
#include "ts_packet.hpp"

namespace {

using testutil::Bytes;

// Test keys of Section 5.2.2.
const std::string kK3 = "3A7F1C9E5B2D8046A1E3C5079B4D6F28";
const std::string kK4 = "C04E92B7165FD83A2E7B0C9145D6A3F1";
const std::string kKX = "00112233445566778899AABBCCDDEEFF";
const std::string kK3L = "3a7f1c9e5b2d8046a1e3c5079b4d6f28";

fakes::AesKey aes_key(const std::string& hex) {
    fakes::AesKey key{};
    for (std::size_t i = 0; i < 16; ++i) key[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return key;
}

// Stream description of the reference set (Section 5.2.1).
al::StreamDescription reference_stream(bool with_audio = true) {
    al::StreamDescription d;
    d.program_number = 1;
    d.pmt_pid = 4096;
    d.pcr_pid = 256;
    d.video_pid = 256;
    d.video_stream_type = 0x1B;
    if (with_audio) {
        d.audio_pid = 257;
        d.audio_stream_type = 0x0F;
    }
    return d;
}

// ---- synthetic video packets on PID 256
std::uint8_t g_cc = 0;

Bytes packet(std::uint16_t pid, bool pusi, Bytes payload, bool pcr = false) {
    testutil::TsPacketSpec spec;
    spec.pid = pid;
    spec.pusi = pusi;
    spec.cc = g_cc++ & 0x0F;
    if (pcr) spec.pcr_base = 90000;
    spec.payload = std::move(payload);
    return testutil::make_ts_packet(spec);
}

Bytes pes_payload(std::size_t n) {
    Bytes p(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(0x30 + i);
    if (n >= 4) {
        p[0] = 0x00;
        p[1] = 0x00;
        p[2] = 0x01;
        p[3] = 0xE0;   // PES start code, video stream_id
    }
    return p;
}

Bytes pes_start() { return packet(256, true, pes_payload(184)); }
Bytes continuation() { return packet(256, false, Bytes(184, 0x5A)); }
Bytes short_pes_start() { return packet(256, true, pes_payload(10)); }    // stays clear (< 16 bytes)
Bytes pcr_only() { return packet(256, false, {}, true); }                 // adaptation field only

// D-04 encryption with the reference implementation (packets with fewer than 16 payload bytes stay clear).
Bytes encrypted(Bytes p, const std::string& key_hex = kK3) {
    fakes::d04_encrypt_packet(aes_key(key_hex), p.data());
    return p;
}

// The test plays AL-SES and AL-OUT: it records the events and the forwarded packets.
struct Harness {
    fakes::CallLog calls;
    fakes::FakeDemux demux{&calls};
    fakes::FakeClock clock;
    fakes::FakeLogger logger;
    al::TaskQueue queue{clock, logger};
    al::KeyManager dsc{demux, logger, queue};

    std::vector<al::Protection> protections;
    std::optional<std::size_t> ok_at;    // number of packets forwarded before EV-KEY-OK
    std::optional<std::size_t> bad_at;   // number of packets forwarded before EV-KEY-BAD
    int ok_count = 0;
    int bad_count = 0;
    std::vector<Bytes> forwarded;
    std::vector<bool> forwarded_scrambled;
    bool release_on_bad = false;         // T-17: AL-SES discards the key and releases

    bool start(const std::string& service = "svc-3", al::StreamDescription stream = reference_stream(),
               std::uint64_t generation = 1) {
        al::KeyManager::Events events;
        events.on_video1 = [this](al::Protection p) { protections.push_back(p); };
        events.on_key_ok = [this] {
            ++ok_count;
            ok_at = forwarded.size();
        };
        events.on_key_bad = [this, service] {
            ++bad_count;
            bad_at = forwarded.size();
            if (release_on_bad) {
                dsc.discard(service);
                dsc.close();
            }
        };
        events.on_packet = [this](const std::uint8_t* p, bool scrambled) {
            forwarded.emplace_back(p, p + al::kTsPacketSize);
            forwarded_scrambled.push_back(scrambled);
        };
        return dsc.start(1, generation, service, stream, events);
    }

    void feed(const Bytes& p) {
        demux.feed(p);
        queue.run_pending();
    }

    bool feed_content(const std::string& name, double seconds) {
        const std::string path = testutil::content_path(name);
        if (!testutil::content_exists(path)) return false;
        const std::size_t count = testutil::packets_for_seconds(seconds);
        for (std::size_t i = 0; i < count; ++i) {
            if (demux.feed_file(path, i, 1) != 1) break;
            queue.run_pending();
        }
        return true;
    }
};

// ---------------------------------------------------------------- TST-111

TEST(Tst111, ProtectionDecision) {
    // (a) all video packets with scrambled = false: clear.
    {
        Harness h;
        ASSERT_TRUE(h.start("svc-1"));
        h.feed(pcr_only());
        h.feed(short_pes_start());
        h.feed(pes_start());
        for (int i = 0; i < 5; ++i) h.feed(continuation());
        ASSERT_EQ(h.protections.size(), 1u);
        EXPECT_EQ(h.protections[0], al::Protection::Clear);
        EXPECT_TRUE(h.logger.contains("protection clear session=1 generation=1"));
        EXPECT_EQ(h.ok_count + h.bad_count, 0);
    }
    // (b) adaptation-only packet and a packet with 10 payload bytes (both '00'), then a packet with
    // 184 payload bytes and '10': protected; the first two are not used for the decision.
    for (bool with_key : {false, true}) {
        SCOPED_TRACE(with_key ? "key stored" : "no key");
        Harness h;
        if (with_key) {
            ASSERT_TRUE(h.dsc.store("svc-3", kK3));
        }
        ASSERT_TRUE(h.start());
        h.feed(pcr_only());
        h.feed(short_pes_start());
        EXPECT_TRUE(h.protections.empty());
        h.feed(encrypted(continuation()));
        for (int i = 0; i < 3; ++i) h.feed(encrypted(continuation()));
        ASSERT_EQ(h.protections.size(), 1u);
        EXPECT_EQ(h.protections[0], with_key ? al::Protection::ProtectedKey : al::Protection::ProtectedNoKey);
        EXPECT_TRUE(h.logger.contains(std::string("protection protected key_set=") + (with_key ? "true" : "false")));
        EXPECT_EQ(h.forwarded.size(), 6u);
    }
}

// ---------------------------------------------------------------- TST-113, TST-114 (synthetic)

std::vector<Bytes> protected_stream() {
    return {pcr_only(),        encrypted(pes_start()), encrypted(continuation()), short_pes_start(),
            encrypted(pes_start()), encrypted(continuation()), encrypted(pes_start()), encrypted(pes_start())};
}

TEST(Tst113, KeyAcceptedAfterThreePesStarts) {
    Harness h;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    ASSERT_TRUE(h.start());
    for (const Bytes& p : protected_stream()) h.feed(p);

    ASSERT_EQ(h.protections.size(), 1u);
    EXPECT_EQ(h.protections[0], al::Protection::ProtectedKey);
    EXPECT_EQ(h.ok_count, 1);
    EXPECT_EQ(h.bad_count, 0);
    ASSERT_TRUE(h.ok_at.has_value());
    EXPECT_EQ(*h.ok_at, 6u);   // at packet 6: PES starts 1, 4 and 6; the short one (3) not counted
    EXPECT_EQ(h.dsc.checked_pes_starts(), 3);
    EXPECT_TRUE(h.logger.contains("key accepted service=svc-3 key=3A7F\xE2\x80\xA6 session=1 generation=1"));
    // The forwarded packets are decrypted (bits '00') and keep the received scrambled flag.
    ASSERT_EQ(h.forwarded.size(), 8u);
    EXPECT_EQ((h.forwarded[1][3] >> 6) & 0x3, 0);
    EXPECT_TRUE(al::pes_start_ok(h.forwarded[1].data()));
    EXPECT_TRUE(h.forwarded_scrambled[1]);
    EXPECT_FALSE(h.forwarded_scrambled[3]);
}

TEST(Tst114, KeyRejectedAtTheFirstPesStart) {
    for (const std::string& wrong : {kKX, kK4}) {   // KX, and K4 entered for svc-3 (Section 5.2.2)
        SCOPED_TRACE(wrong);
        Harness h;
        h.release_on_bad = true;
        ASSERT_TRUE(h.dsc.store("svc-3", wrong));
        ASSERT_TRUE(h.start());
        for (const Bytes& p : protected_stream()) h.feed(p);

        EXPECT_EQ(h.bad_count, 1);
        EXPECT_EQ(h.ok_count, 0);
        ASSERT_TRUE(h.bad_at.has_value());
        EXPECT_EQ(*h.bad_at, 1u);   // the first checked PES start (packet 1)
        EXPECT_EQ(h.dsc.checked_pes_starts(), 0);   // reset by the release of the attempt
        EXPECT_FALSE(h.dsc.has_key("svc-3"));       // discarded (REQ-53)
        EXPECT_NE(h.calls.find("clear_keys"), fakes::CallLog::npos);
        EXPECT_EQ(h.forwarded.size(), 1u);          // nothing after the release
        EXPECT_TRUE(h.logger.contains("key rejected service=svc-3 key=" + wrong.substr(0, 4) + "\xE2\x80\xA6"));
    }
}

// ---------------------------------------------------------------- TST-113, TST-114 (content)

TEST(Tst113, ContentSvc3WithK3) {
    Harness h;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    ASSERT_TRUE(h.start());
    if (!h.feed_content("svc-3.enc.ts", 2.0)) return;
    ASSERT_EQ(h.protections.size(), 1u);
    EXPECT_EQ(h.protections[0], al::Protection::ProtectedKey);
    EXPECT_EQ(h.ok_count, 1);
    EXPECT_EQ(h.bad_count, 0);
    EXPECT_EQ(h.dsc.checked_pes_starts(), 3);
    // Independent count: video PES starts received with scrambled = true before EV-KEY-OK.
    ASSERT_TRUE(h.ok_at.has_value());
    std::size_t scrambled_starts = 0;
    for (std::size_t i = 0; i < *h.ok_at; ++i) {
        if (al::ts_pid(h.forwarded[i].data()) == 256 && al::ts_pusi(h.forwarded[i].data()) && h.forwarded_scrambled[i]) {
            ++scrambled_starts;
        }
    }
    EXPECT_EQ(scrambled_starts, 2u);   // the third one is the packet at which EV-KEY-OK is reported
}

TEST(Tst114, ContentSvc3WithKxAndK4) {
    for (const std::string& wrong : {kKX, kK4}) {
        SCOPED_TRACE(wrong);
        Harness h;
        h.release_on_bad = true;
        ASSERT_TRUE(h.dsc.store("svc-3", wrong));
        ASSERT_TRUE(h.start());
        if (!h.feed_content("svc-3.enc.ts", 2.0)) continue;
        EXPECT_EQ(h.bad_count, 1);
        EXPECT_EQ(h.ok_count, 0);
        EXPECT_FALSE(h.dsc.has_key("svc-3"));
        EXPECT_NE(h.calls.find("clear_keys"), fakes::CallLog::npos);
        // EV-KEY-BAD at the first checked PES start: no video PES start with scrambled = true before it.
        ASSERT_TRUE(h.bad_at.has_value());
        for (std::size_t i = 0; i < *h.bad_at; ++i) {
            EXPECT_FALSE(al::ts_pid(h.forwarded[i].data()) == 256 && al::ts_pusi(h.forwarded[i].data()) &&
                         h.forwarded_scrambled[i]);
        }
    }
}

// ---------------------------------------------------------------- TST-116

TEST(Tst116, NoFullKeyInLogs) {
    fakes::FakeLogger logger;   // shared by the scenarios of TST-112 – TST-115
    {
        Harness h;
        for (const std::string& k : {kK3, kK3L, std::string("3A7F1C9E5B2D8046A1E3C5079B4D6F2"),
                                     std::string("3A7F1C9E5B2D8046A1E3C5079B4D6F2G")}) {
            h.dsc.store("svc-3", k);
        }
        h.dsc.store("svc-4", kK4);
        h.dsc.discard("svc-4");
        ASSERT_TRUE(h.start());
        for (const Bytes& p : protected_stream()) h.feed(p);   // accepted with K3
        for (const auto& line : h.logger.lines()) logger.log(line.level, line.module.c_str(), line.message);
    }
    for (const std::string& wrong : {kKX, kK4}) {
        Harness h;
        h.release_on_bad = true;
        h.dsc.store("svc-3", wrong);
        ASSERT_TRUE(h.start());
        for (const Bytes& p : protected_stream()) h.feed(p);   // rejected
        for (const auto& line : h.logger.lines()) logger.log(line.level, line.module.c_str(), line.message);
    }

    // Patterns of Section 5.2.2: upper- and lower-case hexadecimal strings and the 16 raw bytes.
    std::vector<std::string> patterns;
    for (const std::string& k : {kK3, kK4}) {
        std::string lower = k;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const fakes::AesKey raw = aes_key(k);
        patterns.push_back(k);
        patterns.push_back(lower);
        patterns.emplace_back(raw.begin(), raw.end());
    }
    std::size_t key_lines = 0;
    for (const auto& line : logger.lines()) {
        for (const std::string& p : patterns) EXPECT_EQ(line.message.find(p), std::string::npos) << line.message;
        // Every "key=" is followed by exactly 4 upper-case hexadecimal characters and "…".
        for (std::size_t at = line.message.find("key="); at != std::string::npos;
             at = line.message.find("key=", at + 1)) {
            if (at > 0 && line.message[at - 1] == '_') continue;   // key_set=
            ++key_lines;
            const std::string value = line.message.substr(at + 4, 7);
            ASSERT_EQ(value.size(), 7u) << line.message;
            for (int i = 0; i < 4; ++i) {
                EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(value[i])) &&
                            !std::islower(static_cast<unsigned char>(value[i])))
                    << line.message;
            }
            EXPECT_EQ(value.substr(4), "\xE2\x80\xA6") << line.message;
        }
    }
    EXPECT_GE(key_lines, 6u);   // stored, accepted and rejected lines were checked
    EXPECT_TRUE(logger.contains("3A7F\xE2\x80\xA6"));
    EXPECT_TRUE(logger.contains("C04E\xE2\x80\xA6"));
}

// ---------------------------------------------------------------- TST-105 (TS filters), REQ-49

TEST(Tst105, TsFiltersAfterKey) {
    {
        Harness h;
        ASSERT_TRUE(h.dsc.store("svc-3", kK3));
        ASSERT_TRUE(h.start());
        EXPECT_TRUE(h.dsc.key_set());
        const std::vector<std::string> expected = {
            "set_key 256,257",
            "open_filter ts pid=0 -> 1",    "start_filter 1",
            "open_filter ts pid=4096 -> 2", "start_filter 2",
            "open_filter ts pid=256 -> 3",  "start_filter 3",
            "open_filter ts pid=257 -> 4",  "start_filter 4"};
        EXPECT_EQ(h.calls.entries(), expected);
        EXPECT_TRUE(h.demux.has_key(256));
        EXPECT_TRUE(h.demux.has_key(257));
    }
    {
        Harness h;   // no key stored: no set_key
        ASSERT_TRUE(h.start("svc-1"));
        EXPECT_FALSE(h.dsc.key_set());
        EXPECT_EQ(h.calls.find("set_key"), fakes::CallLog::npos);
        EXPECT_EQ(h.calls.entries().size(), 8u);
    }
    {
        Harness h;   // a stream without audio: key only for the video PID, no filter on 257
        ASSERT_TRUE(h.dsc.store("svc-3", kK3));
        ASSERT_TRUE(h.start("svc-3", reference_stream(false)));
        EXPECT_EQ(h.calls.entries().front(), "set_key 256");
        EXPECT_EQ(h.calls.find("open_filter ts pid=257"), fakes::CallLog::npos);
        EXPECT_EQ(h.demux.open_filters().size(), 3u);
    }
}

TEST(Tst105, PacketsForwardedInOrder) {
    Harness h;
    ASSERT_TRUE(h.dsc.store("svc-3", kK3));
    ASSERT_TRUE(h.start());
    const Bytes clear_video = pes_start();
    const std::vector<Bytes> input = {
        packet(0, true, Bytes(184, 0x00)), packet(4096, true, Bytes(184, 0x02)), encrypted(clear_video),
        packet(300, false, Bytes(184, 0x11)), packet(257, true, Bytes(184, 0x0F)),
        packet(8191, false, Bytes(184, 0xFF)), packet(0, true, Bytes(184, 0x00))};
    for (const Bytes& p : input) h.feed(p);

    const std::vector<std::uint16_t> expected_pids = {0, 4096, 256, 257, 0};
    ASSERT_EQ(h.forwarded.size(), expected_pids.size());
    for (std::size_t i = 0; i < expected_pids.size(); ++i) {
        EXPECT_EQ(al::ts_pid(h.forwarded[i].data()), expected_pids[i]) << i;
    }
    EXPECT_EQ(h.forwarded[2], clear_video);   // decrypted by the descrambler with K3
    EXPECT_EQ(h.forwarded[0], input[0]);
}

TEST(Tst105, DscGenerationCloseAndFailure) {
    {
        Harness h;   // a packet of generation 1 queued, then a new attempt before the worker runs
        ASSERT_TRUE(h.start("svc-1", reference_stream(), 1));
        h.demux.feed(pes_start());
        ASSERT_TRUE(h.start("svc-1", reference_stream(), 2));
        h.queue.run_pending();
        EXPECT_TRUE(h.forwarded.empty());
        EXPECT_TRUE(h.protections.empty());
    }
    {
        Harness h;   // close(): 4 filters closed, clear_keys, nothing afterwards
        ASSERT_TRUE(h.start("svc-1"));
        h.demux.feed(pes_start());
        h.dsc.close();
        h.queue.run_pending();
        EXPECT_TRUE(h.forwarded.empty());
        EXPECT_TRUE(h.demux.open_filters().empty());
        const std::vector<std::string> entries = h.calls.entries();
        EXPECT_EQ(std::vector<std::string>(entries.end() - 5, entries.end()),
                  (std::vector<std::string>{"close_filter 1", "close_filter 2", "close_filter 3", "close_filter 4",
                                            "clear_keys"}));
    }
    {
        Harness h;   // open_filter fails: false, one error, nothing left open, keys cleared
        ASSERT_TRUE(h.dsc.store("svc-3", kK3));
        h.demux.fail_open_filters(true);
        EXPECT_FALSE(h.start());
        EXPECT_EQ(h.logger.count(pal::LogLevel::Error), 1u);
        EXPECT_TRUE(h.logger.contains("cannot open the TS filter on PID 0"));
        EXPECT_TRUE(h.demux.open_filters().empty());
        EXPECT_FALSE(h.demux.has_key(256));
        EXPECT_TRUE(h.dsc.has_key("svc-3"));   // the stored key is kept
    }
}

TEST(Tst112, StoreThroughKeyManager) {
    Harness h;
    EXPECT_TRUE(h.dsc.store("svc-3", kK3));
    EXPECT_TRUE(h.dsc.store("svc-3", kK3L));
    EXPECT_EQ(h.dsc.lookup("svc-3"), al::parse_key_hex(kK3));
    EXPECT_FALSE(h.dsc.store("svc-4", "3A7F1C9E5B2D8046A1E3C5079B4D6F2"));    // KF1
    EXPECT_FALSE(h.dsc.store("svc-4", "3A7F1C9E5B2D8046A1E3C5079B4D6F2G"));   // KF2
    EXPECT_FALSE(h.dsc.has_key("svc-4"));   // nothing stored
}

}  // namespace
