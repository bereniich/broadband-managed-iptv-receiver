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

bool pes_start_ok(const std::uint8_t* packet) {
    const std::size_t offset = ts_payload_offset(packet);
    if (offset == 0 || kTsPacketSize - offset < 3) return false;
    return packet[offset] == 0x00 && packet[offset + 1] == 0x00 && packet[offset + 2] == 0x01;
}

}  // namespace al
