// Tests of the test helpers and fakes of the data path (step 3a). Each test carries the ID of the
// first test that relies on the helper or fake (Section 5.3.1, "Names").
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include <string>
#include <vector>

#include "content.hpp"
#include "fakes/call_log.hpp"
#include "fakes/fake_demux.hpp"
#include "fakes/fake_tuner.hpp"
#include "fakes/reference_d04.hpp"
#include "ffmpeg_fixtures.hpp"
#include "psi_builder.hpp"
#include "ts_builder.hpp"

namespace {

using testutil::Bytes;

Bytes hex(const std::string& text) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

fakes::AesBlock block(const std::string& text) {
    const Bytes b = hex(text);
    fakes::AesBlock out{};
    for (std::size_t i = 0; i < 16; ++i) out[i] = b[i];
    return out;
}

// Test keys of Section 5.2.2.
const fakes::AesKey kK3 = block("3A7F1C9E5B2D8046A1E3C5079B4D6F28");

Bytes counting_bytes(std::size_t n, std::uint8_t start = 0) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(start + i);
    return b;
}

Bytes video_packet(const Bytes& payload, std::uint8_t cc = 0, std::uint8_t scrambling = 0) {
    testutil::TsPacketSpec spec;
    spec.pid = 256;
    spec.pusi = true;
    spec.cc = cc;
    spec.scrambling = scrambling;
    spec.payload = payload;
    return testutil::make_ts_packet(spec);
}

struct Received {
    pal::IDemux::FilterId filter;
    Bytes data;
    bool scrambled;
};

pal::IDemux::DataCallback recorder(std::vector<Received>& out) {
    return [&out](const pal::IDemux::FilterData& d) {
        out.push_back(Received{d.filter, Bytes(d.data, d.data + d.len), d.scrambled});
    };
}

// ---------------------------------------------------------------- TS builder (TST-101)

TEST(Tst101, TsBuilderMatchesFfmpegPackets) {
    // PAT packet of FFmpeg: header, pointer_field, section, 0xFF stuffing.
    Bytes expected_pat = testutil::kFfmpegPatPacketHead;
    expected_pat.insert(expected_pat.end(), testutil::kFfmpegPat.begin(), testutil::kFfmpegPat.end());
    expected_pat.resize(188, 0xFF);
    const std::vector<Bytes> pat = testutil::packetize_section(0, testutil::kFfmpegPat, 0);
    ASSERT_EQ(pat.size(), 1u);
    EXPECT_EQ(pat[0], expected_pat);

    // Key-frame packet of FFmpeg: adaptation field with RAI and PCR_base 63000, then the PES start.
    testutil::TsPacketSpec spec;
    spec.pid = 256;
    spec.pusi = true;
    spec.rai = true;
    spec.pcr_base = 63000;
    spec.payload = {0x00, 0x00, 0x01, 0xe0};
    spec.payload.resize(176, 0x00);   // 188 - 4 header - 8 adaptation field
    const Bytes packet = testutil::make_ts_packet(spec);
    ASSERT_EQ(packet.size(), 188u);
    EXPECT_EQ(Bytes(packet.begin(), packet.begin() + 16), testutil::kFfmpegKeyFrameHead);
}

TEST(Tst101, TsBuilderAdaptationFieldShapes) {
    testutil::TsPacketSpec spec;
    spec.pid = 257;
    spec.payload = counting_bytes(10);   // short payload: adaptation field with stuffing
    const Bytes short_payload = testutil::make_ts_packet(spec);
    ASSERT_EQ(short_payload.size(), 188u);
    EXPECT_EQ((short_payload[3] >> 4) & 0x3, 0x3);    // '11'
    EXPECT_EQ(short_payload[4], 188 - 4 - 10 - 1);     // adaptation_field_length
    EXPECT_EQ(Bytes(short_payload.end() - 10, short_payload.end()), counting_bytes(10));

    spec.payload.clear();
    const Bytes no_payload = testutil::make_ts_packet(spec);
    EXPECT_EQ((no_payload[3] >> 4) & 0x3, 0x2);        // '10': adaptation field only
    EXPECT_EQ(no_payload[4], 183);

    spec.payload = counting_bytes(183);                // exactly one byte of adaptation field
    const Bytes one_byte_af = testutil::make_ts_packet(spec);
    EXPECT_EQ(one_byte_af[4], 0);
    EXPECT_EQ(one_byte_af.size(), 188u);
}

// ---------------------------------------------------------------- reference D-04 (TST-113)

TEST(Tst113, ReferenceAesKnownAnswers) {
    // FIPS-197, Appendix C.1 (AES-128).
    const fakes::AesKey key = block("000102030405060708090a0b0c0d0e0f");
    EXPECT_EQ(fakes::aes128_encrypt_block(key, block("00112233445566778899aabbccddeeff")),
              block("69c4e0d86a7b0430d8cdb78070b4c55a"));
    EXPECT_EQ(fakes::aes128_decrypt_block(key, block("69c4e0d86a7b0430d8cdb78070b4c55a")),
              block("00112233445566778899aabbccddeeff"));

    // NIST SP 800-38A, F.2.1 (CBC-AES128.Encrypt) and F.2.2 (CBC-AES128.Decrypt).
    const fakes::AesKey cbc_key = block("2b7e151628aed2a6abf7158809cf4f3c");
    const fakes::AesBlock iv = block("000102030405060708090a0b0c0d0e0f");
    const Bytes plaintext = hex(
        "6bc1bee22e409f96e93d7e117393172a" "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef" "f69f2445df4f9b17ad2b417be66c3710");
    const Bytes ciphertext = hex(
        "7649abac8119b246cee98e9b12e9197d" "5086cb9b507219ee95db113a917678b2"
        "73bed6b8e3c1743b7116e69e22229516" "3ff1caa1681fac09120eca307586e1a7");
    Bytes data = plaintext;
    fakes::cbc_encrypt(cbc_key, iv, data.data(), data.size());
    EXPECT_EQ(data, ciphertext);
    fakes::cbc_decrypt(cbc_key, iv, data.data(), data.size());
    EXPECT_EQ(data, plaintext);
}

TEST(Tst113, ReferenceD04Packet) {
    const fakes::AesKey key = block("000102030405060708090a0b0c0d0e0f");
    // 4-byte header, payload of 184 bytes: the FIPS-197 C.1 plaintext followed by 168 bytes.
    Bytes payload = hex("00112233445566778899aabbccddeeff");
    const Bytes tail = counting_bytes(168, 0x40);
    payload.insert(payload.end(), tail.begin(), tail.end());
    const Bytes clear = video_packet(payload);
    Bytes packet = clear;

    ASSERT_TRUE(fakes::d04_encrypt_packet(key, packet.data()));
    EXPECT_EQ((packet[3] >> 6) & 0x3, 0x2);                                   // '10'
    EXPECT_EQ(Bytes(packet.begin(), packet.begin() + 3), Bytes(clear.begin(), clear.begin() + 3));
    // First CBC block with IV = 0 is the plain AES block of FIPS-197 C.1.
    EXPECT_EQ(Bytes(packet.begin() + 4, packet.begin() + 20), hex("69c4e0d86a7b0430d8cdb78070b4c55a"));
    // 176 bytes encrypted, the residual 8 bytes stay clear.
    EXPECT_EQ(Bytes(packet.begin() + 180, packet.end()), Bytes(clear.begin() + 180, clear.end()));

    ASSERT_TRUE(fakes::d04_decrypt_packet(key, packet.data()));
    EXPECT_EQ(packet, clear);   // bits back to '00', payload restored

    // A payload shorter than 16 bytes is never encrypted (Section 4.7.3).
    testutil::TsPacketSpec spec;
    spec.pid = 256;
    spec.payload = counting_bytes(10);
    Bytes short_packet = testutil::make_ts_packet(spec);
    const Bytes short_copy = short_packet;
    EXPECT_FALSE(fakes::d04_encrypt_packet(key, short_packet.data()));
    EXPECT_EQ(short_packet, short_copy);

    // Adaptation field only: no payload.
    spec.payload.clear();
    Bytes af_only = testutil::make_ts_packet(spec);
    EXPECT_FALSE(fakes::d04_encrypt_packet(key, af_only.data()));

    // Exactly 16 payload bytes: one block, no residual.
    spec.payload = counting_bytes(16);
    Bytes sixteen = testutil::make_ts_packet(spec);
    const Bytes sixteen_copy = sixteen;
    EXPECT_TRUE(fakes::d04_encrypt_packet(key, sixteen.data()));
    EXPECT_NE(Bytes(sixteen.end() - 16, sixteen.end()), Bytes(sixteen_copy.end() - 16, sixteen_copy.end()));

    // Decryption changes only packets with '10'.
    Bytes clear_copy = clear;
    EXPECT_FALSE(fakes::d04_decrypt_packet(key, clear_copy.data()));
    EXPECT_EQ(clear_copy, clear);
}

// ---------------------------------------------------------------- fake demux (TST-105, TST-113)

TEST(Tst105, FakeDemuxSectionAssembly) {
    fakes::FakeDemux demux;
    std::vector<Received> pat_out;
    std::vector<Received> pmt_out;
    const auto pat_filter = demux.open_filter({pal::FilterType::Section, 0, 0x00}, recorder(pat_out));
    const auto pmt_filter = demux.open_filter({pal::FilterType::Section, 4096, 0x02}, recorder(pmt_out));
    ASSERT_TRUE(demux.start_filter(pat_filter));
    ASSERT_TRUE(demux.start_filter(pmt_filter));

    // One section in one packet.
    const Bytes pat = testutil::make_pat({{1, 4096}});
    demux.feed(testutil::join(testutil::packetize_section(0, pat, 0)));
    ASSERT_EQ(pat_out.size(), 1u);
    EXPECT_EQ(pat_out[0].data, pat);
    EXPECT_FALSE(pat_out[0].scrambled);

    // One section over 3 packets (program_info of 2 descriptors of 202 bytes -> 421-byte section).
    Bytes program_info;
    for (int d = 0; d < 2; ++d) {
        program_info.push_back(0x80);
        program_info.push_back(200);
        const Bytes body = counting_bytes(200, static_cast<std::uint8_t>(d));
        program_info.insert(program_info.end(), body.begin(), body.end());
    }
    const Bytes long_pmt = testutil::make_pmt(1, 256, {{0x1B, 256, {}}, {0x0F, 257, {}}}, program_info);
    const std::vector<Bytes> long_packets = testutil::packetize_section(4096, long_pmt, 0);
    ASSERT_EQ(long_packets.size(), 3u);
    demux.feed(testutil::join(long_packets));
    ASSERT_EQ(pmt_out.size(), 1u);
    EXPECT_EQ(pmt_out[0].data, long_pmt);

    // Two sections in one packet.
    const Bytes pat2 = testutil::make_pat({{2, 768}}, 1, 1);
    Bytes payload = {0x00};
    payload.insert(payload.end(), pat.begin(), pat.end());
    payload.insert(payload.end(), pat2.begin(), pat2.end());
    payload.resize(184, 0xFF);
    testutil::TsPacketSpec spec;
    spec.pid = 0;
    spec.pusi = true;
    spec.cc = 1;
    spec.payload = payload;
    demux.feed(testutil::make_ts_packet(spec));
    ASSERT_EQ(pat_out.size(), 3u);
    EXPECT_EQ(pat_out[1].data, pat);
    EXPECT_EQ(pat_out[2].data, pat2);

    // pointer_field > 0: the end of a section and the start of the next one in the same packet.
    const Bytes first_part(long_pmt.begin(), long_pmt.begin() + 183);
    const Bytes rest(long_pmt.begin() + 183, long_pmt.end());
    Bytes p1 = {0x00};
    p1.insert(p1.end(), first_part.begin(), first_part.end());
    Bytes p2 = rest;                                          // remaining 238 bytes do not fit:
    p2.resize(183);                                           // a middle packet without pusi
    Bytes p3 = {static_cast<std::uint8_t>(rest.size() - 183)};
    p3.insert(p3.end(), rest.begin() + 183, rest.end());
    const Bytes short_pmt = testutil::make_pmt(1, 256, {{0x1B, 256, {}}});
    p3.insert(p3.end(), short_pmt.begin(), short_pmt.end());
    p3.resize(184, 0xFF);
    spec.pid = 4096;
    spec.pusi = true;
    spec.cc = 3;
    spec.payload = p1;
    demux.feed(testutil::make_ts_packet(spec));
    spec.pusi = false;
    spec.cc = 4;
    spec.payload = p2;
    demux.feed(testutil::make_ts_packet(spec));
    spec.pusi = true;
    spec.cc = 5;
    spec.payload = p3;
    demux.feed(testutil::make_ts_packet(spec));
    ASSERT_EQ(pmt_out.size(), 3u);
    EXPECT_EQ(pmt_out[1].data, long_pmt);
    EXPECT_EQ(pmt_out[2].data, short_pmt);

    // table_id filter: a PAT section on a filter for 0x02 is not delivered.
    std::vector<Received> wrong_out;
    const auto wrong = demux.open_filter({pal::FilterType::Section, 0, 0x02}, recorder(wrong_out));
    demux.start_filter(wrong);
    demux.feed(testutil::join(testutil::packetize_section(0, pat, 2)));
    EXPECT_TRUE(wrong_out.empty());
    EXPECT_EQ(pat_out.size(), 4u);

    // B3: a section longer than 4 096 bytes is discarded; the next one is delivered.
    Bytes huge = {0x02, 0xBF, 0xFF};   // section_length 4095 -> 4098 bytes
    huge.resize(4098, 0x00);
    demux.feed(testutil::join(testutil::packetize_section(4096, huge, 6)));
    demux.feed(testutil::join(testutil::packetize_section(4096, short_pmt, 0)));
    ASSERT_EQ(pmt_out.size(), 4u);
    EXPECT_EQ(pmt_out[3].data, short_pmt);
}

TEST(Tst105, FakeDemuxFilterLifecycle) {
    fakes::CallLog log;
    fakes::FakeDemux demux(&log);
    std::vector<Received> a;
    std::vector<Received> b;
    const Bytes packet = video_packet(counting_bytes(184), 0);

    EXPECT_EQ(demux.open_filter({pal::FilterType::Ts, 0x2000, -1}, recorder(a)), 0u);
    const auto fa = demux.open_filter({pal::FilterType::Ts, 256, -1}, recorder(a));
    const auto fb = demux.open_filter({pal::FilterType::Ts, 256, -1}, recorder(b));
    ASSERT_NE(fa, 0u);
    ASSERT_NE(fb, 0u);

    demux.feed(packet);   // not started: nothing
    EXPECT_TRUE(a.empty());

    demux.start_filter(fa);
    demux.start_filter(fb);
    demux.feed(video_packet(counting_bytes(184), 1));
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);   // both filters of the PID receive the packet
    EXPECT_EQ(a[0].data.size(), 188u);
    EXPECT_EQ(a[0].data, video_packet(counting_bytes(184), 1));
    EXPECT_FALSE(a[0].scrambled);
    EXPECT_NE(a[0].data.data(), b[0].data.data());

    // Other PIDs are not delivered.
    testutil::TsPacketSpec other;
    other.pid = 257;
    other.payload = counting_bytes(184);
    demux.feed(testutil::make_ts_packet(other));
    EXPECT_EQ(a.size(), 1u);

    demux.stop_filter(fa);
    demux.feed(video_packet(counting_bytes(184), 2));
    EXPECT_EQ(a.size(), 1u);
    EXPECT_EQ(b.size(), 2u);

    demux.close_filter(fb);
    demux.feed(video_packet(counting_bytes(184), 3));
    EXPECT_EQ(b.size(), 2u);

    // Sync byte and continuity counter (statistics of IDemux).
    demux.start_filter(fa);
    Bytes bad_sync = video_packet(counting_bytes(184), 4);
    bad_sync[0] = 0x46;
    demux.feed(bad_sync);
    EXPECT_EQ(a.size(), 1u);
    demux.feed(video_packet(counting_bytes(184), 9));   // cc 3 -> 9: discontinuity, still delivered
    EXPECT_EQ(a.size(), 2u);
    const pal::IDemux::Stats stats = demux.stats();
    EXPECT_EQ(stats.ts_packets, 7u);
    EXPECT_EQ(stats.sync_errors, 1u);
    EXPECT_EQ(stats.cc_errors, 1u);

    // A callback that closes its own filter gets no further data.
    std::vector<Received> c;
    pal::IDemux::FilterId fc = 0;
    fc = demux.open_filter({pal::FilterType::Ts, 256, -1}, [&](const pal::IDemux::FilterData& d) {
        c.push_back(Received{d.filter, Bytes(d.data, d.data + d.len), d.scrambled});
        demux.close_filter(fc);
    });
    demux.start_filter(fc);
    demux.feed(video_packet(counting_bytes(184), 10));
    demux.feed(video_packet(counting_bytes(184), 11));
    EXPECT_EQ(c.size(), 1u);

    const std::vector<std::string> expected_head = {
        "open_filter ts pid=8192 -> 0", "open_filter ts pid=256 -> 1", "open_filter ts pid=256 -> 2",
        "start_filter 1", "start_filter 2", "stop_filter 1", "close_filter 2", "start_filter 1"};
    const std::vector<std::string> entries = log.entries();
    ASSERT_GE(entries.size(), expected_head.size());
    EXPECT_EQ(std::vector<std::string>(entries.begin(), entries.begin() + 8), expected_head);
}

TEST(Tst113, FakeDemuxDescrambling) {
    fakes::CallLog log;
    fakes::FakeDemux demux(&log);
    std::vector<Received> out;
    const auto f = demux.open_filter({pal::FilterType::Ts, 256, -1}, recorder(out));
    demux.start_filter(f);

    const Bytes clear = video_packet(counting_bytes(184, 7), 0);
    Bytes encrypted = clear;
    ASSERT_TRUE(fakes::d04_encrypt_packet(kK3, encrypted.data()));

    // Key set for the PID: decrypted, bits '00', scrambled = true (REQ-50).
    const std::uint16_t pids[] = {256, 257};
    demux.set_key(pids, 2, kK3.data());
    EXPECT_TRUE(demux.has_key(256));
    demux.feed(encrypted);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].data, clear);
    EXPECT_TRUE(out[0].scrambled);

    // Clear packet: unchanged, scrambled = false.
    demux.feed(clear);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].data, clear);
    EXPECT_FALSE(out[1].scrambled);

    // After clear_keys: unchanged, bits '10', scrambled = true.
    demux.clear_keys();
    EXPECT_FALSE(demux.has_key(256));
    demux.feed(encrypted);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[2].data, encrypted);
    EXPECT_EQ((out[2].data[3] >> 6) & 0x3, 0x2);
    EXPECT_TRUE(out[2].scrambled);

    // A key of another PID does not apply.
    const std::uint16_t audio_only[] = {257};
    demux.set_key(audio_only, 1, kK3.data());
    demux.feed(encrypted);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[3].data, encrypted);

    // The call log never contains key bytes (REQ-55).
    for (const std::string& e : log.entries()) {
        EXPECT_EQ(e.find("3A7F"), std::string::npos) << e;
        EXPECT_EQ(e.find("3a7f"), std::string::npos) << e;
    }
    EXPECT_NE(log.find("set_key 256,257"), fakes::CallLog::npos);
    EXPECT_NE(log.find("clear_keys"), fakes::CallLog::npos);
}

TEST(Tst113, ContentMissingFails) {
    const std::string missing = testutil::content_path("no-such-file.ts");
    EXPECT_NONFATAL_FAILURE(EXPECT_FALSE(testutil::content_exists(missing)),
                            "content missing: run content/generate.sh");
    fakes::FakeDemux demux;
    std::size_t fed = 1;
    EXPECT_NONFATAL_FAILURE(fed = demux.feed_file(missing, 0, 10),
                            "content missing: run content/generate.sh");
    EXPECT_EQ(fed, 0u);
}

// ---------------------------------------------------------------- fake tuner (TST-131)

TEST(Tst131, FakeTunerStatus) {
    fakes::CallLog log;
    fakes::FakeTuner tuner(&log);
    std::vector<pal::TunerStatus> statuses;
    tuner.set_status_handler([&](pal::TunerStatus s) { statuses.push_back(s); });

    EXPECT_FALSE(tuner.emit(pal::TunerStatus::Locked));   // not tuned
    EXPECT_TRUE(tuner.tune({5004, 3054235519u}));
    EXPECT_TRUE(tuner.emit(pal::TunerStatus::Locked));
    tuner.untune();
    EXPECT_FALSE(tuner.emit(pal::TunerStatus::Unlocked));   // no status after untune() (rule 4)
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses[0], pal::TunerStatus::Locked);

    tuner.set_tune_result(false);
    EXPECT_FALSE(tuner.tune({5004, 1u}));
    EXPECT_FALSE(tuner.emit(pal::TunerStatus::Locked));

    EXPECT_EQ(log.entries(), (std::vector<std::string>{"tune 5004 3054235519", "untune", "tune 5004 1"}));
    EXPECT_EQ(tuner.tune_calls(), 2);
}

}  // namespace
