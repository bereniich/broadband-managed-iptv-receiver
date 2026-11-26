// Test helper (Section 5.3.1, "Test helpers"): builds TS packets (ISO/IEC 13818-1, 2.4.3.2 and
// 2.4.3.4) for the AL unit tests. It is not part of the code under test and uses none of it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "psi_builder.hpp"

namespace testutil {

constexpr std::size_t kTsPacketSize = 188;

struct TsPacketSpec {
    std::uint16_t pid = 0;
    bool pusi = false;                         // payload_unit_start_indicator
    std::uint8_t cc = 0;                       // continuity_counter (4 bits)
    std::uint8_t scrambling = 0;               // transport_scrambling_control (2 bits), 2 = '10'
    bool adaptation = false;                   // force an adaptation field even without PCR / RAI
    bool rai = false;                          // random_access_indicator
    std::optional<std::uint64_t> pcr_base;     // 33 bits, 90 kHz
    std::uint16_t pcr_ext = 0;                 // 9 bits, 27 MHz
    Bytes payload;                             // at most 184 bytes minus the adaptation field
};

// One 188-byte packet. The adaptation field is present when `adaptation`, `rai` or `pcr_base` is set
// or when the payload is shorter than 184 bytes; it is filled with stuffing bytes 0xFF up to 188.
// adaptation_field_control is '01', '11' or '10' (no payload).
Bytes make_ts_packet(const TsPacketSpec& spec);

// Splits one PSI section into packets: pointer_field 0 in the first packet (pusi = 1), continuation
// packets without pointer_field, the last packet filled with 0xFF; continuity counters from `cc`.
std::vector<Bytes> packetize_section(std::uint16_t pid, const Bytes& section, std::uint8_t cc = 0);

// Concatenates packets into one byte stream.
Bytes join(const std::vector<Bytes>& packets);

}  // namespace testutil
