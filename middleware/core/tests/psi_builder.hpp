// Test helper (Section 5.3.1, "Test helpers"): builds PAT and PMT sections for the AL unit tests.
// It is not part of the code under test and uses none of it; its CRC-32 is a separate bitwise
// implementation, checked against the standard check value 0x0376E6E7.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace testutil {

using Bytes = std::vector<std::uint8_t>;

// CRC-32/MPEG-2, bit by bit (no table).
std::uint32_t crc32_mpeg2_bitwise(const std::uint8_t* data, std::size_t len);
std::uint32_t crc32_mpeg2_bitwise(const std::string& text);

struct PatProgram {
    std::uint16_t program_number;
    std::uint16_t pid;   // PMT PID, or the network PID for program_number 0
};

struct PmtStream {
    std::uint8_t stream_type;
    std::uint16_t pid;
    Bytes es_info;   // descriptors
};

// Complete sections (table_id … CRC_32) with section_syntax_indicator 1, current_next_indicator 1,
// section_number 0, last_section_number 0 and all reserved bits 1 (ISO/IEC 13818-1, 2.4.4).
Bytes make_pat(const std::vector<PatProgram>& programs, std::uint16_t transport_stream_id = 1,
               std::uint8_t version = 0, std::uint8_t table_id = 0x00);
Bytes make_pmt(std::uint16_t program_number, std::uint16_t pcr_pid,
               const std::vector<PmtStream>& streams, const Bytes& program_info = {},
               std::uint8_t version = 0, std::uint8_t table_id = 0x02);

}  // namespace testutil
