#include "ts_packet.hpp"

namespace al {

std::uint16_t ts_pid(const std::uint8_t* packet) {
    return static_cast<std::uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
}

bool ts_pusi(const std::uint8_t* packet) { return (packet[1] & 0x40) != 0; }

std::size_t ts_payload_offset(const std::uint8_t* packet) {
    switch ((packet[3] >> 4) & 0x3) {
        case 0x1:
            return 4;
        case 0x3: {
            const std::size_t offset = 5 + std::size_t{packet[4]};
            return offset < kTsPacketSize ? offset : 0;
        }
        default:
            return 0;
    }
}

std::size_t ts_payload_size(const std::uint8_t* packet) {
    const std::size_t offset = ts_payload_offset(packet);
    return offset == 0 ? 0 : kTsPacketSize - offset;
}

namespace {

// Length of the adaptation field after its length byte, 0 when there is none.
std::size_t adaptation_field_length(const std::uint8_t* packet) {
    const std::uint8_t afc = (packet[3] >> 4) & 0x3;
    if (afc != 0x2 && afc != 0x3) return 0;
    const std::size_t length = packet[4];
    return length <= kTsPacketSize - 5 ? length : 0;
}

}  // namespace

std::optional<std::uint64_t> ts_pcr(const std::uint8_t* packet) {
    const std::size_t length = adaptation_field_length(packet);
    if (length < 7 || (packet[5] & 0x10) == 0) return std::nullopt;   // PCR_flag, 6 PCR bytes
    const std::uint8_t* p = packet + 6;
    const std::uint64_t base = (std::uint64_t{p[0]} << 25) | (std::uint64_t{p[1]} << 17) |
                               (std::uint64_t{p[2]} << 9) | (std::uint64_t{p[3]} << 1) | (p[4] >> 7);
    const std::uint64_t extension = ((std::uint64_t{p[4]} & 0x01) << 8) | p[5];
    return base * 300 + extension;
}

bool ts_random_access(const std::uint8_t* packet) {
    return adaptation_field_length(packet) >= 1 && (packet[5] & 0x40) != 0;
}

bool pes_start_ok(const std::uint8_t* packet) {
    const std::size_t offset = ts_payload_offset(packet);
    if (offset == 0 || kTsPacketSize - offset < 3) return false;
    return packet[offset] == 0x00 && packet[offset + 1] == 0x00 && packet[offset + 2] == 0x01;
}

}  // namespace al
