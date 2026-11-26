// Unit tests of HAL-DSC (Section 5.3.3): TST-215 – TST-217 (REQ-50, D-04).
// Expected values: FIPS-197 C.1 and NIST SP 800-38A F.2.2 (TST-215), the clear content file and the
// file packaged by SRV-PKG, an independent implementation of D-04 (TST-216, TST-217), and packets
// encrypted by the test helper as Section 4.7.3 describes.
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "demux.hpp"
#include "descrambler.hpp"
#include "ts_helper.hpp"

namespace {

using hal::Demux;
using hal::Descrambler;
using testhelp::Bytes;
using testhelp::Packet;

Bytes hex(const std::string& text) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return out;
}

const char* kK3 = "3A7F1C9E5B2D8046A1E3C5079B4D6F28";  // Section 5.2.2

int scrambling(const std::uint8_t* p) { return (p[3] >> 6) & 0x3; }

// ---------------------------------------------------------------------------------------- TST-215

TEST(Tst215, Fips197C1ThroughTheInternalCbcFunctionWithZeroIv) {
    const Bytes key = hex("000102030405060708090a0b0c0d0e0f");
    const Bytes cipher = hex("69c4e0d86a7b0430d8cdb78070b4c55a");
    const std::uint8_t zero_iv[16] = {};
    Bytes plain(16);
    ASSERT_TRUE(Descrambler::aes128_cbc_decrypt(key.data(), zero_iv, cipher.data(), plain.data(), 16));
    EXPECT_EQ(plain, hex("00112233445566778899aabbccddeeff"));
}

TEST(Tst215, NistSp80038aF22CbcDecryptionWithItsIv) {
    const Bytes key = hex("2b7e151628aed2a6abf7158809cf4f3c");
    const Bytes iv = hex("000102030405060708090a0b0c0d0e0f");
    const Bytes cipher = hex(
        "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
        "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    const Bytes expected = hex(
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    Bytes plain(64);
    ASSERT_TRUE(Descrambler::aes128_cbc_decrypt(key.data(), iv.data(), cipher.data(), plain.data(), 64));
    EXPECT_EQ(plain, expected);
    Bytes in_place = cipher;  // the same buffer for input and output
    ASSERT_TRUE(Descrambler::aes128_cbc_decrypt(key.data(), iv.data(), in_place.data(), in_place.data(), 64));
    EXPECT_EQ(in_place, expected);
    EXPECT_FALSE(Descrambler::aes128_cbc_decrypt(key.data(), iv.data(), cipher.data(), plain.data(), 15));
    EXPECT_FALSE(Descrambler::aes128_cbc_decrypt(nullptr, iv.data(), cipher.data(), plain.data(), 16));
}

// ---------------------------------------------------------------------------------------- TST-217
// with packets built in the test (all rules of REQ-50, no content file needed)

struct Delivered {
    Bytes data;
    bool scrambled;
};

TEST(Tst217, BuiltPacketsFollowTheRulesOfReq50) {
    const auto k3 = testhelp::key_from_hex(kK3);
    Demux demux;
    std::vector<Delivered> a, b, audio;
    auto sink = [](std::vector<Delivered>& v) {
        return [&v](const std::uint8_t* d, std::size_t n, bool s) { v.push_back({Bytes(d, d + n), s}); };
    };
    const auto fa = demux.add_filter(Demux::FilterType::Ts, 256, -1, sink(a));
    const auto fb = demux.add_filter(Demux::FilterType::Ts, 256, -1, sink(b));  // a second filter, same bytes
    const auto fc = demux.add_filter(Demux::FilterType::Ts, 257, -1, sink(audio));
    demux.start_filter(fa);
    demux.start_filter(fb);
    demux.start_filter(fc);

    // Clear packets: payload only (184 bytes: 11 blocks + 8 bytes tail), adaptation field of 7 bytes
    // (payload 176 bytes: 11 blocks, no tail), adaptation field of 13 bytes (170: 10 blocks + 10), a
    // payload of 10 bytes (never encrypted), and the same payload twice (the IV restarts per packet).
    std::uint8_t cc = 0;
    std::vector<Packet> clear = {testhelp::packet(256, cc++, false, Bytes(184, 0x5A)),
                                 testhelp::packet_with_adaptation(256, cc++, 7, 1),
                                 testhelp::packet_with_adaptation(256, cc++, 13, 2),
                                 testhelp::packet_with_adaptation(256, cc++, 173, 3),
                                 testhelp::packet(256, cc++, false, Bytes(184, 0x5A))};
    std::vector<Packet> sent = clear;
    std::vector<bool> encrypted;
    for (Packet& p : sent) encrypted.push_back(testhelp::d04_encrypt(p, k3));
    EXPECT_EQ(encrypted, (std::vector<bool>{true, true, true, false, true}));

    // Without a key: unchanged, '10', scrambled = true (TST-217).
    std::vector<Packet> input = sent;
    for (Packet& p : input) demux.feed(p.data(), 1);
    ASSERT_EQ(a.size(), sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) {
        EXPECT_EQ(a[i].data, Bytes(sent[i].begin(), sent[i].end())) << i;
        EXPECT_EQ(a[i].scrambled, encrypted[i]) << i;
    }
    a.clear();
    b.clear();

    // With K3 for PID 256: decrypted, bits '00', scrambled = true; the 10-byte payload stays as sent;
    // both filters get the same bytes; PID 257 has no key.
    const std::uint16_t video[] = {256};
    ASSERT_TRUE(demux.set_key(video, 1, k3.data()));
    input = sent;
    for (Packet& p : input) demux.feed(p.data(), 1);
    ASSERT_EQ(a.size(), sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) {
        EXPECT_EQ(a[i].data, Bytes(clear[i].begin(), clear[i].end())) << i;
        EXPECT_EQ(scrambling(a[i].data.data()), 0) << i;
        EXPECT_EQ(a[i].scrambled, encrypted[i]) << i;
        EXPECT_EQ(b[i].data, a[i].data) << i;
    }
    EXPECT_EQ(demux.stats().packets_descrambled, 4u);
    Packet audio_packet = testhelp::packet(257, 0, false, Bytes(184, 0x33));
    ASSERT_TRUE(testhelp::d04_encrypt(audio_packet, k3));
    const Packet audio_sent = audio_packet;
    demux.feed(audio_packet.data(), 1);
    ASSERT_EQ(audio.size(), 1u);
    EXPECT_EQ(audio[0].data, Bytes(audio_sent.begin(), audio_sent.end()));
    EXPECT_TRUE(audio[0].scrambled);

    // Scrambling values other than '10' are not D-04: delivered unchanged, scrambled = true.
    for (std::uint8_t bits : {0x40, 0xC0}) {
        a.clear();
        Packet odd = testhelp::packet(256, cc++, false, Bytes(184, 0x11));
        odd[3] = static_cast<std::uint8_t>((odd[3] & 0x3F) | bits);
        const Packet before = odd;
        demux.feed(odd.data(), 1);
        ASSERT_EQ(a.size(), 1u);
        EXPECT_EQ(a[0].data, Bytes(before.begin(), before.end()));
        EXPECT_TRUE(a[0].scrambled);
    }

    // The same rule on the descrambler itself (the demux also calls it only for '10').
    {
        Descrambler direct;
        ASSERT_TRUE(direct.set_key(video, 1, k3.data()));
        for (std::uint8_t bits : {0x00, 0x40, 0xC0}) {
            Packet p = sent[0];
            p[3] = static_cast<std::uint8_t>((p[3] & 0x3F) | bits);
            const Packet before = p;
            EXPECT_FALSE(direct.descramble(p.data())) << static_cast<int>(bits);
            EXPECT_EQ(p, before) << static_cast<int>(bits);
        }
        Packet p = sent[0];
        EXPECT_TRUE(direct.descramble(p.data()));
        EXPECT_EQ(p, clear[0]);
    }

    // After clear_keys: unchanged again.
    demux.clear_keys();
    a.clear();
    input = sent;
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i][3] = static_cast<std::uint8_t>((input[i][3] & 0xF0) | (cc & 0x0F));
        ++cc;
    }
    const std::vector<Packet> expected_after = input;
    for (Packet& p : input) demux.feed(p.data(), 1);
    ASSERT_EQ(a.size(), sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) EXPECT_EQ(a[i].data, Bytes(expected_after[i].begin(), expected_after[i].end())) << i;
    EXPECT_EQ(demux.stats().packets_descrambled, 4u);
}

// ---------------------------------------------------------------------------------------- content

std::string content_path(const char* name) { return std::string(M15_REPO_DIR) + "/content/" + name; }

// Reads `count` packets from `in` into `out`; returns the number read.
std::size_t read_packets(std::ifstream& in, std::vector<std::uint8_t>& out, std::size_t count) {
    out.resize(count * 188);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    const std::size_t n = static_cast<std::size_t>(in.gcount()) / 188;
    out.resize(n * 188);
    return n;
}

bool open_content(std::ifstream& clear, std::ifstream& enc) {
    clear.open(content_path("svc-3.ts"), std::ios::binary);
    enc.open(content_path("svc-3.enc.ts"), std::ios::binary);
    if (!clear || !enc) {
        ADD_FAILURE() << "content missing: run content/generate.sh (and package svc-3.enc.ts with package_ts): "
                      << content_path(!clear ? "svc-3.ts" : "svc-3.enc.ts");
        return false;
    }
    return true;
}

// TST-216: svc-3.enc.ts through demux and descrambler with K3 for PIDs 256 and 257 equals svc-3.ts
// packet for packet; scrambled = true exactly for the packets received with '10'.
TEST(Tst216, PackagedFileDecryptsToTheClearFile) {
    std::ifstream clear, enc;
    if (!open_content(clear, enc)) return;
    const auto k3 = testhelp::key_from_hex(kK3);
    Demux demux;
    std::vector<std::uint8_t> clear_chunk, enc_chunk;
    std::size_t index = 0, chunk_base = 0, mismatches = 0, wrong_flags = 0, scrambled_in = 0;
    std::vector<bool> was_scrambled;
    // One TS filter per PID: every packet is delivered once, in the order it was fed.
    for (std::uint16_t pid = 0; pid <= 0x1FFF; ++pid) {
        const auto id = demux.add_filter(Demux::FilterType::Ts, pid, -1, [&](const std::uint8_t* d, std::size_t n, bool s) {
            const std::size_t k = index - chunk_base;
            if (n != 188 || std::memcmp(d, clear_chunk.data() + k * 188, 188) != 0) ++mismatches;
            if (s != was_scrambled[k]) ++wrong_flags;
            ++index;
        });
        demux.start_filter(id);
    }
    const std::uint16_t av[] = {256, 257};
    ASSERT_TRUE(demux.set_key(av, 2, k3.data()));
    for (;;) {
        const std::size_t n = read_packets(enc, enc_chunk, 7000);
        const std::size_t m = read_packets(clear, clear_chunk, 7000);
        ASSERT_EQ(n, m) << "the files differ in size";
        if (n == 0) break;
        was_scrambled.assign(n, false);
        for (std::size_t k = 0; k < n; ++k) {
            was_scrambled[k] = scrambling(enc_chunk.data() + k * 188) != 0;
            if (scrambling(enc_chunk.data() + k * 188) == 0x2) ++scrambled_in;
        }
        chunk_base = index;
        demux.feed(enc_chunk.data(), n);
        ASSERT_EQ(index, chunk_base + n) << "a packet was not delivered";
    }
    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(wrong_flags, 0u);
    EXPECT_GT(scrambled_in, 100000u) << "svc-3.enc.ts contains too few encrypted packets";
    EXPECT_EQ(demux.stats().packets_descrambled, scrambled_in);
    EXPECT_EQ(demux.stats().sync_errors, 0u);
}

// TST-217 with the content: 1 000 packets without a key and 1 000 after clear_keys() are delivered
// unchanged ('10' stays, scrambled = true).
TEST(Tst217, PackagedPacketsWithoutKeyAndAfterClearKeysStayUnchanged) {
    std::ifstream clear, enc;
    if (!open_content(clear, enc)) return;
    const auto k3 = testhelp::key_from_hex(kK3);
    Demux demux;
    std::vector<Delivered> got;
    for (std::uint16_t pid : {256, 257}) {
        const auto id = demux.add_filter(Demux::FilterType::Ts, pid, -1,
                                         [&](const std::uint8_t* d, std::size_t n, bool s) { got.push_back({Bytes(d, d + n), s}); });
        demux.start_filter(id);
    }
    // The first 1 000 encrypted packets of the file, then the next 1 000.
    std::vector<std::uint8_t> chunk;
    std::vector<Packet> first, second;
    while ((first.size() < 1000 || second.size() < 1000) && read_packets(enc, chunk, 1) == 1) {
        if (scrambling(chunk.data()) != 0x2) continue;
        Packet p;
        std::copy(chunk.begin(), chunk.end(), p.begin());
        (first.size() < 1000 ? first : second).push_back(p);
    }
    ASSERT_EQ(first.size(), 1000u);
    ASSERT_EQ(second.size(), 1000u);
    auto feed_and_check = [&](const std::vector<Packet>& packets) {
        got.clear();
        demux.reset_stats();  // a new stream for the continuity counters
        for (Packet p : packets) demux.feed(p.data(), 1);
        ASSERT_EQ(got.size(), packets.size());
        for (std::size_t i = 0; i < packets.size(); ++i) {
            EXPECT_EQ(got[i].data, Bytes(packets[i].begin(), packets[i].end())) << i;
            EXPECT_TRUE(got[i].scrambled) << i;
            EXPECT_EQ(scrambling(got[i].data.data()), 0x2) << i;
        }
    };
    feed_and_check(first);  // without a key
    const std::uint16_t av[] = {256, 257};
    ASSERT_TRUE(demux.set_key(av, 2, k3.data()));
    {
        got.clear();
        Packet p = first[0];
        demux.feed(p.data(), 1);
        ASSERT_EQ(got.size(), 1u);
        EXPECT_EQ(scrambling(got[0].data.data()), 0);  // the key works
    }
    demux.clear_keys();
    feed_and_check(second);  // after clear_keys
}

}  // namespace
