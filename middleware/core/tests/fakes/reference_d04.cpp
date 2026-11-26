#include "reference_d04.hpp"

namespace fakes {

namespace {

constexpr std::size_t kPacketSize = 188;

// GF(2^8) with the AES polynomial x^8 + x^4 + x^3 + x + 1 (FIPS-197, 4.2).
std::uint8_t xtime(std::uint8_t b) {
    return static_cast<std::uint8_t>((b << 1) ^ ((b & 0x80) ? 0x1B : 0x00));
}

std::uint8_t gmul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t result = 0;
    while (b != 0) {
        if (b & 1) result ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return result;
}

struct SBoxes {
    std::array<std::uint8_t, 256> forward{};
    std::array<std::uint8_t, 256> inverse{};
};

std::uint8_t rotl8(std::uint8_t b, int n) {
    return static_cast<std::uint8_t>((b << n) | (b >> (8 - n)));
}

// S-box of FIPS-197, 5.1.1: multiplicative inverse in GF(2^8) followed by the affine transformation.
SBoxes make_sboxes() {
    SBoxes s;
    for (int x = 0; x < 256; ++x) {
        std::uint8_t inv = 0;
        if (x != 0) {
            for (int y = 1; y < 256; ++y) {
                if (gmul(static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)) == 1) {
                    inv = static_cast<std::uint8_t>(y);
                    break;
                }
            }
        }
        const std::uint8_t b = inv;
        const std::uint8_t v =
            static_cast<std::uint8_t>(b ^ rotl8(b, 1) ^ rotl8(b, 2) ^ rotl8(b, 3) ^ rotl8(b, 4) ^ 0x63);
        s.forward[static_cast<std::size_t>(x)] = v;
        s.inverse[v] = static_cast<std::uint8_t>(x);
    }
    return s;
}

const SBoxes& sboxes() {
    static const SBoxes s = make_sboxes();
    return s;
}

using RoundKeys = std::array<std::uint8_t, 176>;   // 11 round keys of 16 bytes (FIPS-197, 5.2)

RoundKeys expand_key(const AesKey& key) {
    const auto& sbox = sboxes().forward;
    RoundKeys w{};
    for (std::size_t i = 0; i < 16; ++i) w[i] = key[i];
    std::uint8_t rcon = 0x01;
    for (std::size_t i = 16; i < 176; i += 4) {
        std::uint8_t t[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
        if (i % 16 == 0) {
            // RotWord, SubWord, Rcon
            const std::uint8_t first = t[0];
            t[0] = static_cast<std::uint8_t>(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = xtime(rcon);
        }
        for (std::size_t j = 0; j < 4; ++j) {
            w[i + j] = static_cast<std::uint8_t>(w[i + j - 16] ^ t[j]);
        }
    }
    return w;
}

// The state is stored column by column: byte (row r, column c) is s[r + 4c] (FIPS-197, 3.4).
void add_round_key(AesBlock& s, const RoundKeys& w, std::size_t round) {
    for (std::size_t i = 0; i < 16; ++i) s[i] ^= w[16 * round + i];
}

void shift_rows(AesBlock& s, bool inverse) {
    const AesBlock in = s;
    for (std::size_t r = 1; r < 4; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            const std::size_t from = inverse ? (c + 4 - r) % 4 : (c + r) % 4;
            s[r + 4 * c] = in[r + 4 * from];
        }
    }
}

void mix_columns(AesBlock& s, bool inverse) {
    const std::uint8_t m0 = inverse ? 0x0E : 0x02, m1 = inverse ? 0x0B : 0x03;
    const std::uint8_t m2 = inverse ? 0x0D : 0x01, m3 = inverse ? 0x09 : 0x01;
    for (std::size_t c = 0; c < 4; ++c) {
        const std::uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
        s[4 * c] = static_cast<std::uint8_t>(gmul(a0, m0) ^ gmul(a1, m1) ^ gmul(a2, m2) ^ gmul(a3, m3));
        s[4 * c + 1] = static_cast<std::uint8_t>(gmul(a0, m3) ^ gmul(a1, m0) ^ gmul(a2, m1) ^ gmul(a3, m2));
        s[4 * c + 2] = static_cast<std::uint8_t>(gmul(a0, m2) ^ gmul(a1, m3) ^ gmul(a2, m0) ^ gmul(a3, m1));
        s[4 * c + 3] = static_cast<std::uint8_t>(gmul(a0, m1) ^ gmul(a1, m2) ^ gmul(a2, m3) ^ gmul(a3, m0));
    }
}

void sub_bytes(AesBlock& s, const std::array<std::uint8_t, 256>& box) {
    for (auto& b : s) b = box[b];
}

std::uint8_t scrambling_bits(const std::uint8_t* packet) { return (packet[3] >> 6) & 0x3; }

void set_scrambling_bits(std::uint8_t* packet, std::uint8_t bits) {
    packet[3] = static_cast<std::uint8_t>((packet[3] & 0x3F) | ((bits & 0x3) << 6));
}

constexpr AesBlock kZeroIv{};

}  // namespace

AesBlock aes128_encrypt_block(const AesKey& key, const AesBlock& plaintext) {
    const RoundKeys w = expand_key(key);
    AesBlock s = plaintext;
    add_round_key(s, w, 0);
    for (std::size_t round = 1; round <= 10; ++round) {
        sub_bytes(s, sboxes().forward);
        shift_rows(s, false);
        if (round != 10) mix_columns(s, false);
        add_round_key(s, w, round);
    }
    return s;
}

AesBlock aes128_decrypt_block(const AesKey& key, const AesBlock& ciphertext) {
    // Inverse cipher of FIPS-197, 5.3.
    const RoundKeys w = expand_key(key);
    AesBlock s = ciphertext;
    add_round_key(s, w, 10);
    for (std::size_t round = 9;; --round) {
        shift_rows(s, true);
        sub_bytes(s, sboxes().inverse);
        add_round_key(s, w, round);
        if (round == 0) break;
        mix_columns(s, true);
    }
    return s;
}

void cbc_encrypt(const AesKey& key, const AesBlock& iv, std::uint8_t* data, std::size_t len) {
    AesBlock previous = iv;
    for (std::size_t offset = 0; offset + 16 <= len; offset += 16) {
        AesBlock block;
        for (std::size_t i = 0; i < 16; ++i) block[i] = static_cast<std::uint8_t>(data[offset + i] ^ previous[i]);
        previous = aes128_encrypt_block(key, block);
        for (std::size_t i = 0; i < 16; ++i) data[offset + i] = previous[i];
    }
}

void cbc_decrypt(const AesKey& key, const AesBlock& iv, std::uint8_t* data, std::size_t len) {
    AesBlock previous = iv;
    for (std::size_t offset = 0; offset + 16 <= len; offset += 16) {
        AesBlock cipher;
        for (std::size_t i = 0; i < 16; ++i) cipher[i] = data[offset + i];
        const AesBlock plain = aes128_decrypt_block(key, cipher);
        for (std::size_t i = 0; i < 16; ++i) data[offset + i] = static_cast<std::uint8_t>(plain[i] ^ previous[i]);
        previous = cipher;
    }
}

std::size_t ts_payload_offset(const std::uint8_t* packet) {
    const std::uint8_t afc = (packet[3] >> 4) & 0x3;
    if (afc == 0x1) return 4;
    if (afc == 0x3) {
        const std::size_t offset = 5 + std::size_t{packet[4]};
        return offset < kPacketSize ? offset : 0;
    }
    return 0;   // '00' reserved, '10' adaptation field only
}

bool d04_encrypt_packet(const AesKey& key, std::uint8_t* packet) {
    const std::size_t o = ts_payload_offset(packet);
    if (o == 0) return false;
    const std::size_t n = (kPacketSize - o) / 16 * 16;
    if (n == 0) return false;   // fewer than 16 payload bytes: stays clear with '00' (Section 4.7.3)
    cbc_encrypt(key, kZeroIv, packet + o, n);
    set_scrambling_bits(packet, 0x2);
    return true;
}

bool d04_decrypt_packet(const AesKey& key, std::uint8_t* packet) {
    if (scrambling_bits(packet) != 0x2) return false;
    const std::size_t o = ts_payload_offset(packet);
    const std::size_t n = o == 0 ? 0 : (kPacketSize - o) / 16 * 16;
    cbc_decrypt(key, kZeroIv, packet + o, n);
    set_scrambling_bits(packet, 0x0);
    return true;
}

}  // namespace fakes
