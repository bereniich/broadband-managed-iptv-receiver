// Test helper (Section 5.3.1, "Test helpers"): builds TS packets, PSI sections and RTP datagrams in
// the test code. Not part of the code under test; its CRC-32/MPEG-2 is checked against the standard
// check value 0x0376E6E7 of the ASCII string "123456789" (TST-213).
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace testhelp {

using Packet = std::array<std::uint8_t, 188>;
using Bytes = std::vector<std::uint8_t>;

// CRC-32/MPEG-2: polynomial 0x04C11DB7, initial value 0xFFFFFFFF, no reflection, no final XOR.
inline std::uint32_t crc32_mpeg2(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

// A long-form PSI section of exactly `total` bytes (8-byte header, filler, CRC_32), total 12 … 4 098.
inline Bytes section(std::uint8_t table_id, std::size_t total, std::uint8_t filler_seed = 0) {
    if (total < 12 || total > 4098) throw std::invalid_argument("section size");
    const std::size_t length = total - 3;  // section_length (12 bits)
    Bytes s;
    s.push_back(table_id);
    s.push_back(static_cast<std::uint8_t>(0xB0 | ((length >> 8) & 0x0F)));  // syntax 1, '0', reserved '11'
    s.push_back(static_cast<std::uint8_t>(length & 0xFF));
    s.push_back(0x00);
    s.push_back(0x01);  // table_id_extension
    s.push_back(0xC1);  // reserved, version 0, current_next 1
    s.push_back(0x00);  // section_number
    s.push_back(0x00);  // last_section_number
    for (std::size_t i = 8; i < total - 4; ++i) s.push_back(static_cast<std::uint8_t>(filler_seed + i));
    const std::uint32_t crc = crc32_mpeg2(s.data(), s.size());
    s.push_back(static_cast<std::uint8_t>(crc >> 24));
    s.push_back(static_cast<std::uint8_t>(crc >> 16));
    s.push_back(static_cast<std::uint8_t>(crc >> 8));
    s.push_back(static_cast<std::uint8_t>(crc));
    return s;
}

// A TS packet with a 4-byte header and up to 184 payload bytes; the rest is 0xFF (PSI stuffing).
inline Packet packet(std::uint16_t pid, std::uint8_t cc, bool unit_start, const Bytes& payload = {},
                     std::uint8_t sync = 0x47) {
    if (payload.size() > 184) throw std::invalid_argument("payload size");
    Packet p;
    p.fill(0xFF);
    p[0] = sync;
    p[1] = static_cast<std::uint8_t>((unit_start ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    p[2] = static_cast<std::uint8_t>(pid & 0xFF);
    p[3] = static_cast<std::uint8_t>(0x10 | (cc & 0x0F));  // payload only
    for (std::size_t i = 0; i < payload.size(); ++i) p[4 + i] = payload[i];
    return p;
}

// A packet with an adaptation field only (no payload); `discontinuity` sets the indicator.
inline Packet adaptation_only_packet(std::uint16_t pid, std::uint8_t cc, bool discontinuity = false) {
    Packet p;
    p.fill(0xFF);
    p[0] = 0x47;
    p[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1F);
    p[2] = static_cast<std::uint8_t>(pid & 0xFF);
    p[3] = static_cast<std::uint8_t>(0x20 | (cc & 0x0F));
    p[4] = 183;
    p[5] = discontinuity ? 0x80 : 0x00;
    return p;
}

// A packet with an adaptation field that carries the discontinuity_indicator, and a payload.
inline Packet discontinuity_packet(std::uint16_t pid, std::uint8_t cc) {
    Packet p = packet(pid, cc, false);
    p[3] = static_cast<std::uint8_t>(0x30 | (cc & 0x0F));
    p[4] = 1;
    p[5] = 0x80;
    return p;
}

// Splits a PSI byte stream into packets of `pid`: the stream starts with a section (the first packet
// gets payload_unit_start_indicator = 1 and pointer_field 0); further unit starts are given as
// offsets in the stream where a section begins in a later packet. Continuity counters from `cc`.
inline std::vector<Packet> packetize(std::uint16_t pid, std::uint8_t& cc, const Bytes& stream,
                                     const std::vector<std::size_t>& section_starts) {
    std::vector<Packet> out;
    std::size_t pos = 0;
    bool first = true;
    while (pos < stream.size()) {
        // A packet has a unit start if a section begins in its payload range.
        std::size_t next_start = SIZE_MAX;
        for (std::size_t s : section_starts) {
            if (s >= pos && s < pos + 183 && s < next_start) next_start = s;
        }
        if (first) next_start = 0;
        Bytes payload;
        const bool pusi = next_start != SIZE_MAX;
        for (std::size_t s : section_starts) {
            if (!pusi && s == pos + 183) throw std::invalid_argument("section start at payload byte 184");
        }
        if (pusi) payload.push_back(static_cast<std::uint8_t>(next_start - pos));
        const std::size_t room = 184 - payload.size();
        const std::size_t n = std::min(room, stream.size() - pos);
        payload.insert(payload.end(), stream.begin() + static_cast<std::ptrdiff_t>(pos),
                       stream.begin() + static_cast<std::ptrdiff_t>(pos + n));
        out.push_back(packet(pid, cc, pusi, payload));
        cc = static_cast<std::uint8_t>((cc + 1) & 0x0F);
        pos += n;
        first = false;
    }
    return out;
}

// ---- D-04 (test side) --------------------------------------------------------------------------

// 16 key bytes from 32 hexadecimal characters (Section 5.2.2).
inline std::array<std::uint8_t, 16> key_from_hex(const char* hex) {
    std::array<std::uint8_t, 16> key{};
    for (std::size_t i = 0; i < 16; ++i) {
        auto nibble = [](char c) { return static_cast<std::uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
        key[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    }
    return key;
}

// Encrypts one packet as the packaging tool does (Section 4.7.3), written here independently of the
// code under test: payload offset o (4, or 5 + adaptation_field_length), n = the largest multiple of
// 16 bytes, AES-128-CBC with IV 0 and a new context per packet, no padding; bits '10' if n > 0.
// Returns false (packet unchanged) if nothing is encrypted.
inline bool d04_encrypt(Packet& p, const std::array<std::uint8_t, 16>& key) {
    const int afc = (p[3] >> 4) & 0x3;
    if (afc == 0x0 || afc == 0x2) return false;
    const std::size_t o = afc == 0x1 ? 4 : 5 + p[4];
    if (o >= 188) return false;
    const std::size_t n = (188 - o) / 16 * 16;
    if (n == 0) return false;
    const std::uint8_t iv[16] = {};
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::uint8_t out[188];
    int l1 = 0, l2 = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key.data(), iv);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_EncryptUpdate(ctx, out, &l1, p.data() + o, static_cast<int>(n));
    EVP_EncryptFinal_ex(ctx, out + l1, &l2);
    EVP_CIPHER_CTX_free(ctx);
    std::copy(out, out + n, p.begin() + static_cast<std::ptrdiff_t>(o));
    p[3] = static_cast<std::uint8_t>((p[3] & 0x3F) | 0x80);
    return true;
}

// A packet with an adaptation field of `af_length` bytes (stuffing) and a payload of 184 - 1 - af_length
// bytes filled from `seed`.
inline Packet packet_with_adaptation(std::uint16_t pid, std::uint8_t cc, std::uint8_t af_length, std::uint8_t seed) {
    Packet p;
    p.fill(0xFF);
    p[0] = 0x47;
    p[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1F);
    p[2] = static_cast<std::uint8_t>(pid & 0xFF);
    p[3] = static_cast<std::uint8_t>(0x30 | (cc & 0x0F));
    p[4] = af_length;
    if (af_length > 0) p[5] = 0x00;  // flags
    for (std::size_t i = 5 + af_length; i < 188; ++i) p[i] = static_cast<std::uint8_t>(seed + i * 7);
    return p;
}

// ---- RTP --------------------------------------------------------------------------------------

struct RtpHeader {
    std::uint8_t byte0 = 0x80;  // V = 2
    std::uint8_t payload_type = 33;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
};

inline Bytes rtp_datagram(const RtpHeader& h, const std::vector<Packet>& packets) {
    Bytes d = {h.byte0,
               h.payload_type,
               static_cast<std::uint8_t>(h.sequence >> 8),
               static_cast<std::uint8_t>(h.sequence),
               static_cast<std::uint8_t>(h.timestamp >> 24),
               static_cast<std::uint8_t>(h.timestamp >> 16),
               static_cast<std::uint8_t>(h.timestamp >> 8),
               static_cast<std::uint8_t>(h.timestamp),
               static_cast<std::uint8_t>(h.ssrc >> 24),
               static_cast<std::uint8_t>(h.ssrc >> 16),
               static_cast<std::uint8_t>(h.ssrc >> 8),
               static_cast<std::uint8_t>(h.ssrc)};
    for (const Packet& p : packets) d.insert(d.end(), p.begin(), p.end());
    return d;
}

// UDP sender to 127.0.0.1:<port>.
class UdpSender {
public:
    UdpSender() : fd_(socket(AF_INET, SOCK_DGRAM, 0)) {}
    ~UdpSender() { close(fd_); }
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    void send(std::uint16_t port, const Bytes& datagram) const {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        (void)sendto(fd_, datagram.data(), datagram.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

private:
    int fd_;
};

}  // namespace testhelp
