#include "psi.hpp"

#include <array>

namespace al {

namespace {

constexpr std::uint8_t kPatTableId = 0x00;
constexpr std::uint8_t kPmtTableId = 0x02;
constexpr std::size_t kMaxSectionLength = 1021;   // ISO/IEC 13818-1: PAT and PMT sections
constexpr std::size_t kCrcSize = 4;
// section_length counts from the byte after it to the end of the CRC:
// PAT: transport_stream_id … last_section_number (5) + CRC;
// PMT: program_number … program_info_length (9) + CRC.
constexpr std::size_t kMinPatSectionLength = 5 + kCrcSize;
constexpr std::size_t kMinPmtSectionLength = 9 + kCrcSize;

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint16_t read_pid(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(((p[0] & 0x1F) << 8) | p[1]);
}

std::uint16_t read_length12(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(((p[0] & 0x0F) << 8) | p[1]);
}

// Checks table_id, section_length and CRC_32. On success `section_end` is the offset of the CRC.
PsiError check_section(const std::uint8_t* s, std::size_t len, std::uint8_t table_id,
                       std::size_t min_section_length, std::size_t& section_end) {
    if (s == nullptr || len < 3) {
        return PsiError::TooShort;
    }
    if (s[0] != table_id) {
        return PsiError::BadTableId;
    }
    const std::size_t section_length = read_length12(s + 1);
    if (section_length < min_section_length || section_length > kMaxSectionLength ||
        3 + section_length > len) {
        return PsiError::BadSectionLength;
    }
    const std::size_t total = 3 + section_length;
    section_end = total - kCrcSize;
    const std::uint32_t stored = (std::uint32_t{s[section_end]} << 24) |
                                 (std::uint32_t{s[section_end + 1]} << 16) |
                                 (std::uint32_t{s[section_end + 2]} << 8) | s[section_end + 3];
    if (crc32_mpeg2(s, section_end) != stored) {
        return PsiError::BadCrc;
    }
    return PsiError::None;
}

}  // namespace

std::uint32_t crc32_mpeg2(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = (crc << 8) ^ kCrcTable[((crc >> 24) ^ data[i]) & 0xFF];
    }
    return crc;
}

const char* to_string(PsiError error) {
    switch (error) {
        case PsiError::None: return "none";
        case PsiError::TooShort: return "too short";
        case PsiError::BadTableId: return "wrong table_id";
        case PsiError::BadSectionLength: return "invalid section_length";
        case PsiError::BadCrc: return "CRC-32 mismatch";
        case PsiError::Malformed: return "malformed section";
        case PsiError::NoProgram: return "no program";
    }
    return "unknown";
}

PatResult parse_pat(const std::uint8_t* s, std::size_t len) {
    PatResult result;
    std::size_t end = 0;
    result.error = check_section(s, len, kPatTableId, kMinPatSectionLength, end);
    if (result.error != PsiError::None) {
        return result;
    }
    result.version = static_cast<std::uint8_t>((s[5] >> 1) & 0x1F);
    constexpr std::size_t kLoopStart = 8;
    if ((end - kLoopStart) % 4 != 0) {
        result.error = PsiError::Malformed;
        return result;
    }
    for (std::size_t i = kLoopStart; i < end; i += 4) {
        const std::uint16_t program_number = read_u16(s + i);
        if (program_number != 0) {   // program_number 0 points to the network PID (NIT)
            result.program_number = program_number;
            result.pmt_pid = read_pid(s + i + 2);
            return result;
        }
    }
    result.error = PsiError::NoProgram;
    return result;
}

PmtResult parse_pmt(const std::uint8_t* s, std::size_t len) {
    PmtResult result;
    std::size_t end = 0;
    result.error = check_section(s, len, kPmtTableId, kMinPmtSectionLength, end);
    if (result.error != PsiError::None) {
        return result;
    }
    result.program_number = read_u16(s + 3);
    result.version = static_cast<std::uint8_t>((s[5] >> 1) & 0x1F);
    result.pcr_pid = read_pid(s + 8);
    std::size_t i = 12 + read_length12(s + 10);   // skip the program_info descriptors
    if (i > end) {
        result.error = PsiError::Malformed;
        return result;
    }
    while (i < end) {
        if (end - i < 5) {   // stream_type, elementary_PID, ES_info_length
            result.error = PsiError::Malformed;
            return result;
        }
        const EsInfo es{read_pid(s + i + 1), s[i]};
        const std::size_t es_info_length = read_length12(s + i + 3);
        i += 5 + es_info_length;
        if (i > end) {
            result.error = PsiError::Malformed;
            return result;
        }
        if (es.stream_type == kStreamTypeH264 && !result.video) {
            result.video = es;
        } else if (es.stream_type == kStreamTypeAacAdts && !result.audio) {
            result.audio = es;
        }
    }
    return result;
}

}  // namespace al
