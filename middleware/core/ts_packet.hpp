// TS packet free functions of the AL (Section 4.2.4; ISO/IEC 13818-1, 2.4.3.2 and 2.4.3.4).
// All functions take one complete 188-byte packet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace al {

constexpr std::size_t kTsPacketSize = 188;
// Packets with fewer payload bytes are never encrypted (D-04, Section 4.7.3) and are not used for the
// protection decision (REQ-44).
constexpr std::size_t kMinEncryptedPayload = 16;

std::uint16_t ts_pid(const std::uint8_t* packet);
bool ts_pusi(const std::uint8_t* packet);   // payload_unit_start_indicator

// Offset of the payload after the TS header and the adaptation field: 4 for adaptation_field_control
// '01', 5 + adaptation_field_length for '11'; 0 when the packet has no payload ('00', '10', or an
// adaptation field that reaches the end of the packet).
std::size_t ts_payload_offset(const std::uint8_t* packet);
std::size_t ts_payload_size(const std::uint8_t* packet);

// PCR of the adaptation field in 27 MHz units (PCR_base · 300 + PCR_extension, ISO/IEC 13818-1,
// 2.4.3.5), or nullopt when the packet carries no PCR.
std::optional<std::uint64_t> ts_pcr(const std::uint8_t* packet);
constexpr std::uint64_t kPcrTicksPerSecond = 27000000;

// random_access_indicator of the adaptation field (set by FFmpeg on every key-frame start).
bool ts_random_access(const std::uint8_t* packet);

// PES start check of D-11: the payload begins with the PES start code 00 00 01.
bool pes_start_ok(const std::uint8_t* packet);

}  // namespace al
