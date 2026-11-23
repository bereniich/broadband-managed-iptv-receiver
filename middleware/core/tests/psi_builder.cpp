#include "psi_builder.hpp"

namespace testutil {

namespace {

void put_u16(Bytes& b, unsigned value) {
    b.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

// 3 reserved bits '111' + 13-bit PID.
void put_pid(Bytes& b, std::uint16_t pid) { put_u16(b, 0xE000u | (pid & 0x1FFFu)); }

// 4 reserved bits '1111' + 12-bit length.
void put_length12(Bytes& b, std::size_t length) { put_u16(b, 0xF000u | (length & 0x0FFFu)); }

// Writes table_id and section_length in front of `body` (which starts after section_length)
// and appends the CRC-32.
Bytes finish_section(std::uint8_t table_id, const Bytes& body) {
    Bytes s;
    s.push_back(table_id);
    // section_syntax_indicator '1', '0', reserved '11', section_length = body + CRC.
    put_u16(s, 0xB000u | ((body.size() + 4) & 0x0FFFu));
    s.insert(s.end(), body.begin(), body.end());
    const std::uint32_t crc = crc32_mpeg2_bitwise(s.data(), s.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        s.push_back(static_cast<std::uint8_t>((crc >> shift) & 0xFF));
    }
    return s;
}

void put_version_and_section_numbers(Bytes& b, std::uint8_t version) {
    b.push_back(static_cast<std::uint8_t>(0xC0 | ((version & 0x1F) << 1) | 0x01));  // current_next 1
    b.push_back(0x00);   // section_number
    b.push_back(0x00);   // last_section_number
}

}  // namespace

std::uint32_t crc32_mpeg2_bitwise(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool in = ((data[i] >> bit) & 1) != 0;
            const bool top = (crc & 0x80000000u) != 0;
            crc <<= 1;
            if (in != top) {
                crc ^= 0x04C11DB7u;
            }
        }
    }
    return crc;
}

std::uint32_t crc32_mpeg2_bitwise(const std::string& text) {
    return crc32_mpeg2_bitwise(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

Bytes make_pat(const std::vector<PatProgram>& programs, std::uint16_t transport_stream_id,
               std::uint8_t version, std::uint8_t table_id) {
    Bytes body;
    put_u16(body, transport_stream_id);
    put_version_and_section_numbers(body, version);
    for (const PatProgram& p : programs) {
        put_u16(body, p.program_number);
        put_pid(body, p.pid);
    }
    return finish_section(table_id, body);
}

Bytes make_pmt(std::uint16_t program_number, std::uint16_t pcr_pid,
               const std::vector<PmtStream>& streams, const Bytes& program_info,
               std::uint8_t version, std::uint8_t table_id) {
    Bytes body;
    put_u16(body, program_number);
    put_version_and_section_numbers(body, version);
    put_pid(body, pcr_pid);
    put_length12(body, program_info.size());
    body.insert(body.end(), program_info.begin(), program_info.end());
    for (const PmtStream& es : streams) {
        body.push_back(es.stream_type);
        put_pid(body, es.pid);
        put_length12(body, es.es_info.size());
        body.insert(body.end(), es.es_info.begin(), es.es_info.end());
    }
    return finish_section(table_id, body);
}

}  // namespace testutil
