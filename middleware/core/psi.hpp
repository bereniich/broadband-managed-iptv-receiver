// PSI free functions of AL-PSI (Section 4.2.4): MPEG-2 CRC-32 and the parsers of the PAT and the PMT
// (REQ-16, REQ-17; section syntax of ISO/IEC 13818-1, 2.4.4). No platform dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace al {

// CRC-32/MPEG-2 (ISO/IEC 13818-1 Annex B): polynomial 0x04C11DB7, initial value 0xFFFFFFFF,
// no reflection, no final XOR. Check value of the ASCII string "123456789": 0x0376E6E7.
std::uint32_t crc32_mpeg2(const std::uint8_t* data, std::size_t len);

enum class PsiError {
    None,
    TooShort,           // fewer than 3 bytes: table_id and section_length cannot be read
    BadTableId,         // not 0x00 (PAT) or 0x02 (PMT)
    BadSectionLength,   // section_length below the minimum, above 1021 or larger than the data
    BadCrc,             // CRC_32 does not match
    Malformed,          // a loop or a descriptor length does not fit into the section
    NoProgram           // PAT without a program_number != 0
};

const char* to_string(PsiError error);

struct PatResult {
    PsiError error = PsiError::None;
    std::uint8_t version = 0;
    std::uint16_t program_number = 0;   // first program with program_number != 0
    std::uint16_t pmt_pid = 0;
};

struct EsInfo {
    std::uint16_t pid = 0;
    std::uint8_t stream_type = 0;
};

constexpr std::uint8_t kStreamTypeH264 = 0x1B;
constexpr std::uint8_t kStreamTypeAacAdts = 0x0F;

struct PmtResult {
    PsiError error = PsiError::None;
    std::uint8_t version = 0;
    std::uint16_t program_number = 0;
    std::uint16_t pcr_pid = 0;
    std::optional<EsInfo> video;   // first elementary stream with stream_type 0x1B
    std::optional<EsInfo> audio;   // first elementary stream with stream_type 0x0F
};

// `section` is one complete section (table_id … CRC_32) as delivered by a section filter;
// bytes after the end given by section_length are ignored. The other fields of a result are
// valid only when `error` is PsiError::None.
PatResult parse_pat(const std::uint8_t* section, std::size_t len);
PmtResult parse_pmt(const std::uint8_t* section, std::size_t len);

}  // namespace al
