// SRV-PKG: packaging according to D-04 (Section 4.7.3).
#include "packager.hpp"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#include <openssl/evp.h>

namespace pkg {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::uint16_t pid_of(const std::uint8_t* packet) {
    return static_cast<std::uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
}

// Offset of the payload in a packet (Section 4.7.3, step 2), or 0 for a packet without a payload:
// adaptation_field_control '01' -> 4, '11' -> 5 + adaptation_field_length, '10' and '00' -> none.
std::size_t payload_offset(const std::uint8_t* packet) {
    const int afc = (packet[3] >> 4) & 0x3;
    if (afc == 0x1) return 4;
    if (afc == 0x3) {
        const std::size_t offset = 5 + static_cast<std::size_t>(packet[4]);
        return offset < kTsPacketSize ? offset : 0;   // an adaptation field filling the packet: no payload
    }
    return 0;
}

struct CipherDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const { EVP_CIPHER_CTX_free(ctx); }
};

}  // namespace

std::optional<Key> parse_key(const std::string& text) {
    if (text.size() != 2 * Key().size()) return std::nullopt;
    Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        const int hi = hex_value(text[2 * i]);
        const int lo = hex_value(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        key[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return key;
}

std::string masked_key(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size() && i < 4; ++i) {
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    }
    return out + "\xE2\x80\xA6";   // "…"
}

std::uint32_t crc32_mpeg2(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

// ---------------------------------------------------------------- PSI

std::vector<std::vector<std::uint8_t>> SectionAssembler::feed(const std::uint8_t* packet) {
    std::vector<std::vector<std::uint8_t>> out;
    const std::size_t offset = payload_offset(packet);
    if (offset == 0) return out;
    const std::uint8_t* payload = packet + offset;
    std::size_t size = kTsPacketSize - offset;
    if (packet[1] & 0x40) {   // payload_unit_start_indicator: a pointer_field precedes the section data
        const std::size_t pointer = payload[0];
        if (1 + pointer > size) {   // invalid pointer_field: restart at the next start
            buffer_.clear();
            collecting_ = false;
            return out;
        }
        if (collecting_) {   // the end of the previous section is before the pointed-to start
            buffer_.insert(buffer_.end(), payload + 1, payload + 1 + pointer);
            take_sections(out);
        }
        buffer_.assign(payload + 1 + pointer, payload + size);
        collecting_ = true;
    } else if (collecting_) {
        buffer_.insert(buffer_.end(), payload, payload + size);
    }
    take_sections(out);
    return out;
}

void SectionAssembler::take_sections(std::vector<std::vector<std::uint8_t>>& out) {
    while (collecting_ && buffer_.size() >= 3) {
        if (buffer_[0] == 0xFF) {   // stuffing: no further section in this packet
            buffer_.clear();
            collecting_ = false;
            return;
        }
        const std::size_t length = 3 + (((buffer_[1] & 0x0F) << 8) | buffer_[2]);
        if (buffer_.size() < length) return;   // continues in the next packet
        std::vector<std::uint8_t> section(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(length));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(length));
        if (length >= 12 && crc32_mpeg2(section.data(), section.size()) == 0) out.push_back(std::move(section));
    }
}

std::optional<std::uint16_t> pmt_pid_of_pat(const std::vector<std::uint8_t>& s) {
    if (s.size() < 12 || s[0] != 0x00) return std::nullopt;
    // program loop: from byte 8 to the CRC_32, 4 bytes per program
    for (std::size_t i = 8; i + 4 <= s.size() - 4; i += 4) {
        const int program_number = (s[i] << 8) | s[i + 1];
        if (program_number != 0) return static_cast<std::uint16_t>(((s[i + 2] & 0x1F) << 8) | s[i + 3]);
    }
    return std::nullopt;
}

std::optional<std::set<std::uint16_t>> av_pids_of_pmt(const std::vector<std::uint8_t>& s) {
    if (s.size() < 16 || s[0] != 0x02) return std::nullopt;
    const std::size_t end = s.size() - 4;   // before CRC_32
    std::size_t i = 12 + (((s[10] & 0x0F) << 8) | s[11]);   // after program_info
    std::set<std::uint16_t> pids;
    while (i + 5 <= end) {
        const std::uint8_t stream_type = s[i];
        const std::uint16_t pid = static_cast<std::uint16_t>(((s[i + 1] & 0x1F) << 8) | s[i + 2]);
        const std::size_t es_info_length = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
        if (stream_type == 0x1B || stream_type == 0x0F) pids.insert(pid);
        i += 5 + es_info_length;
    }
    if (i != end) return std::nullopt;   // the stream loop does not end at the CRC_32
    return pids;
}

// ---------------------------------------------------------------- D-04

bool package_packet(std::uint8_t* packet, const Key& key, const std::set<std::uint16_t>& av_pids,
                    PacketAction& action) {
    action = PacketAction::Copied;
    if (av_pids.count(pid_of(packet)) == 0) return true;
    const std::size_t offset = payload_offset(packet);
    if (offset == 0) return true;
    const std::size_t n = (kTsPacketSize - offset) / 16 * 16;
    if (n == 0) return true;   // fewer than 16 payload bytes: unchanged, bits stay '00'

    std::unique_ptr<EVP_CIPHER_CTX, CipherDeleter> ctx(EVP_CIPHER_CTX_new());
    const std::uint8_t iv[16] = {};
    int out_len = 0;
    int final_len = 0;
    std::uint8_t encrypted[kTsPacketSize];
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key.data(), iv) != 1 ||
        EVP_CIPHER_CTX_set_padding(ctx.get(), 0) != 1 ||
        EVP_EncryptUpdate(ctx.get(), encrypted, &out_len, packet + offset, static_cast<int>(n)) != 1 ||
        EVP_EncryptFinal_ex(ctx.get(), encrypted + out_len, &final_len) != 1 ||
        static_cast<std::size_t>(out_len + final_len) != n) {
        return false;
    }
    std::memcpy(packet + offset, encrypted, n);
    packet[3] = static_cast<std::uint8_t>((packet[3] & 0x3F) | 0x80);   // transport_scrambling_control '10'
    action = PacketAction::Encrypted;
    return true;
}

PackageResult package_file(const std::string& in, const std::string& out, const Key& key) {
    namespace fs = std::filesystem;
    PackageResult result;
    std::error_code ec;
    if (!fs::is_regular_file(in, ec)) {
        result.error = "input file missing: " + in;
        return result;
    }
    const std::uintmax_t size = fs::file_size(in, ec);
    if (ec || size == 0 || size % kTsPacketSize != 0) {
        result.error = "input is not a whole number of 188-byte TS packets: " + in;
        return result;
    }
    std::ifstream input(in, std::ios::binary);
    if (!input) {
        result.error = "cannot read " + in;
        return result;
    }

    // Step 1: PAT and PMT of the input (CRC-32 checked); every packet needs its sync byte.
    std::uint8_t packet[kTsPacketSize];
    SectionAssembler pat_assembler;
    SectionAssembler pmt_assembler;
    std::optional<std::uint16_t> pmt_pid;
    std::uint64_t index = 0;
    while (input.read(reinterpret_cast<char*>(packet), kTsPacketSize)) {
        if (packet[0] != 0x47) {
            result.error = "TS packet " + std::to_string(index) + " without sync byte 0x47 in " + in;
            return result;
        }
        const std::uint16_t pid = pid_of(packet);
        if (!pmt_pid && pid == 0) {
            for (const auto& section : pat_assembler.feed(packet)) {
                if ((pmt_pid = pmt_pid_of_pat(section))) break;
            }
        } else if (pmt_pid && pid == *pmt_pid) {
            for (const auto& section : pmt_assembler.feed(packet)) {
                if (const auto pids = av_pids_of_pmt(section)) {
                    result.av_pids = *pids;
                    result.pmt_pid = *pmt_pid;
                    break;
                }
            }
            if (result.pmt_pid != 0) break;
        }
        ++index;
    }
    if (!pmt_pid) {
        result.error = "no valid PAT in " + in;
        return result;
    }
    if (result.pmt_pid == 0) {
        result.error = "no valid PMT on PID " + std::to_string(*pmt_pid) + " in " + in;
        return result;
    }
    if (result.av_pids.empty()) {
        result.error = "the PMT lists no stream of type 0x1B or 0x0F in " + in;
        return result;
    }

    // Steps 2 and 3: every packet, written under a temporary name, renamed when complete.
    const fs::path target(out);
    const fs::path temporary = target.parent_path() / (".tmp-" + target.filename().string());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        result.error = "cannot write " + temporary.string();
        return result;
    }
    auto fail = [&](const std::string& message) {
        output.close();
        fs::remove(temporary, ec);
        result.error = message;
        result.counts.clear();
        return result;
    };
    input.clear();
    input.seekg(0);
    index = 0;
    while (input.read(reinterpret_cast<char*>(packet), kTsPacketSize)) {
        if (packet[0] != 0x47) {
            return fail("TS packet " + std::to_string(index) + " without sync byte 0x47 in " + in);
        }
        PacketAction action;
        if (!package_packet(packet, key, result.av_pids, action)) return fail("AES-128-CBC encryption failed");
        PidCount& count = result.counts[pid_of(packet)];
        (action == PacketAction::Encrypted ? count.encrypted : count.clear) += 1;
        if (!output.write(reinterpret_cast<const char*>(packet), kTsPacketSize)) {
            return fail("cannot write " + temporary.string());
        }
        result.bytes += kTsPacketSize;
        ++index;
    }
    output.close();
    if (!output || result.bytes != size) return fail("cannot write " + temporary.string());
    fs::rename(temporary, target, ec);
    if (ec) return fail("cannot rename " + temporary.string() + " to " + out + ": " + ec.message());
    result.ok = true;
    return result;
}

}  // namespace pkg
