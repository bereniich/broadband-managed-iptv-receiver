// AL-DSC free functions and key store (step 5): TS payload offset and PES start check (TST-111),
// key format (TST-112), key store per service (TST-115).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "key_store.hpp"
#include "ts_builder.hpp"
#include "ts_packet.hpp"

namespace {

using testutil::Bytes;

// Test keys of Section 5.2.2.
const std::string kK3 = "3A7F1C9E5B2D8046A1E3C5079B4D6F28";
const std::string kK4 = "C04E92B7165FD83A2E7B0C9145D6A3F1";
const std::string kK3L = "3a7f1c9e5b2d8046a1e3c5079b4d6f28";
const std::string kKF1 = "3A7F1C9E5B2D8046A1E3C5079B4D6F2";
const std::string kKF2 = "3A7F1C9E5B2D8046A1E3C5079B4D6F2G";

const al::Key kK3Bytes = {0x3A, 0x7F, 0x1C, 0x9E, 0x5B, 0x2D, 0x80, 0x46,
                          0xA1, 0xE3, 0xC5, 0x07, 0x9B, 0x4D, 0x6F, 0x28};
const al::Key kK4Bytes = {0xC0, 0x4E, 0x92, 0xB7, 0x16, 0x5F, 0xD8, 0x3A,
                          0x2E, 0x7B, 0x0C, 0x91, 0x45, 0xD6, 0xA3, 0xF1};

Bytes packet_with(std::size_t payload_size, bool pusi = false) {
    testutil::TsPacketSpec spec;
    spec.pid = 256;
    spec.pusi = pusi;
    spec.payload = Bytes(payload_size, 0xAB);
    if (payload_size >= 3) spec.payload[0] = spec.payload[1] = 0x00, spec.payload[2] = 0x01;
    return testutil::make_ts_packet(spec);
}

// ---------------------------------------------------------------- TST-111

TEST(Tst111, PayloadOffsetFunctions) {
    // '01': payload directly after the 4-byte header.
    const Bytes full = packet_with(184, true);
    EXPECT_EQ(al::ts_pid(full.data()), 256);
    EXPECT_TRUE(al::ts_pusi(full.data()));
    EXPECT_EQ(al::ts_payload_offset(full.data()), 4u);
    EXPECT_EQ(al::ts_payload_size(full.data()), 184u);

    // '11': 5 + adaptation_field_length (ISO/IEC 13818-1, 2.4.3.4).
    const Bytes ten = packet_with(10);
    EXPECT_EQ(ten[4], 173);
    EXPECT_EQ(al::ts_payload_offset(ten.data()), 178u);
    EXPECT_EQ(al::ts_payload_size(ten.data()), 10u);
    const Bytes sixteen = packet_with(16);
    EXPECT_EQ(al::ts_payload_size(sixteen.data()), 16u);
    EXPECT_GE(al::ts_payload_size(sixteen.data()), al::kMinEncryptedPayload);
    EXPECT_LT(al::ts_payload_size(packet_with(15).data()), al::kMinEncryptedPayload);

    // '10' (adaptation field only) and '00' (reserved): no payload.
    const Bytes af_only = packet_with(0);
    EXPECT_EQ((af_only[3] >> 4) & 0x3, 0x2);
    EXPECT_EQ(al::ts_payload_offset(af_only.data()), 0u);
    Bytes reserved = full;
    reserved[3] = static_cast<std::uint8_t>(reserved[3] & 0xCF);
    EXPECT_EQ(al::ts_payload_offset(reserved.data()), 0u);

    // '11' with an adaptation field that reaches the end of the packet: no payload.
    Bytes no_room = packet_with(10);
    no_room[4] = 183;
    EXPECT_EQ(al::ts_payload_offset(no_room.data()), 0u);
    no_room[4] = 200;   // invalid length
    EXPECT_EQ(al::ts_payload_size(no_room.data()), 0u);
}

TEST(Tst111, PesStartCheck) {
    EXPECT_TRUE(al::pes_start_ok(packet_with(184, true).data()));
    EXPECT_TRUE(al::pes_start_ok(packet_with(10, true).data()));
    Bytes wrong = packet_with(184, true);
    wrong[6] = 0x02;   // 00 00 02
    EXPECT_FALSE(al::pes_start_ok(wrong.data()));
    EXPECT_FALSE(al::pes_start_ok(packet_with(0).data()));
    EXPECT_FALSE(al::pes_start_ok(packet_with(2).data()));
}

// ---------------------------------------------------------------- TST-112

TEST(Tst112, KeyFormat) {
    EXPECT_EQ(al::parse_key_hex(kK3), std::optional<al::Key>(kK3Bytes));
    EXPECT_EQ(al::parse_key_hex(kK3L), std::optional<al::Key>(kK3Bytes));   // case-insensitive
    EXPECT_EQ(al::parse_key_hex(kK4), std::optional<al::Key>(kK4Bytes));
    EXPECT_FALSE(al::parse_key_hex(kKF1).has_value());   // 31 characters
    EXPECT_FALSE(al::parse_key_hex(kKF2).has_value());   // non-hexadecimal character
    EXPECT_FALSE(al::parse_key_hex(kK3 + "0").has_value());
    EXPECT_FALSE(al::parse_key_hex("").has_value());
    EXPECT_FALSE(al::parse_key_hex(" 3A7F1C9E5B2D8046A1E3C5079B4D6F2").has_value());
}

TEST(Tst112, KeyMask) {
    EXPECT_EQ(al::mask_key(kK3Bytes), "3A7F\xE2\x80\xA6");
    EXPECT_EQ(al::mask_key(*al::parse_key_hex(kK3L)), "3A7F\xE2\x80\xA6");   // always upper case
    EXPECT_EQ(al::mask_key(kK4Bytes), "C04E\xE2\x80\xA6");
}

// ---------------------------------------------------------------- TST-115

TEST(Tst115, KeysBoundToTheirService) {
    al::KeyStore store;
    std::vector<al::Key> zeroed;
    store.set_zeroed_hook([&](const al::Key& k) { zeroed.push_back(k); });

    store.store("svc-3", kK3Bytes);
    EXPECT_FALSE(store.lookup("svc-4").has_value());   // 1: no key for svc-4

    store.store("svc-4", kK4Bytes);
    store.discard("svc-4");                            // 2: rejection of the key of svc-4
    EXPECT_FALSE(store.lookup("svc-4").has_value());
    ASSERT_EQ(zeroed.size(), 1u);
    EXPECT_EQ(zeroed[0], al::Key{});                   // its 16 bytes were overwritten with zeros
    EXPECT_EQ(store.lookup("svc-3"), std::optional<al::Key>(kK3Bytes));   // 3: K3 still stored

    // Replacing a key zeroes the old bytes; clear_all() zeroes every entry.
    store.store("svc-3", kK4Bytes);
    EXPECT_EQ(zeroed.size(), 2u);
    EXPECT_EQ(store.lookup("svc-3"), std::optional<al::Key>(kK4Bytes));
    store.store("svc-1", kK3Bytes);
    store.clear_all();
    EXPECT_EQ(zeroed.size(), 4u);
    for (const al::Key& k : zeroed) EXPECT_EQ(k, al::Key{});
    EXPECT_EQ(store.size(), 0u);

    al::Key local = kK3Bytes;
    al::zero_key(local);
    EXPECT_EQ(local, al::Key{});
}

}  // namespace
