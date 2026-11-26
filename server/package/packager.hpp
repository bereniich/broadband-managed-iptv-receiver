// SRV-PKG: packaging of a clear MPEG-TS file according to D-04 (Section 4.7.3, REQ-63).
// Own PAT / PMT parser and AES-128-CBC encryption with OpenSSL libcrypto; no code is shared with the
// receiver (rule R5, Section 4.1.4).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pkg {

constexpr std::size_t kTsPacketSize = 188;

using Key = std::array<std::uint8_t, 16>;

// A key of exactly 32 hexadecimal characters (case-insensitive, Section 3.1), or nullopt.
std::optional<Key> parse_key(const std::string& text);

// A key in a log line: its first 4 characters (upper case) followed by "…" (REQ-55).
std::string masked_key(const std::string& text);

// CRC-32 of MPEG-2 systems (ISO/IEC 13818-1, Annex A): polynomial 0x04C11DB7, initial value
// 0xFFFFFFFF, no reflection, no final XOR. A section including its CRC_32 field gives 0.
std::uint32_t crc32_mpeg2(const std::uint8_t* data, std::size_t len);

// Assembles the PSI sections of one PID from its TS packets (pointer_field, sections over several
// packets). Returns the complete sections whose CRC-32 is correct.
class SectionAssembler {
public:
    std::vector<std::vector<std::uint8_t>> feed(const std::uint8_t* packet);

private:
    std::vector<std::uint8_t> buffer_;
    bool collecting_ = false;
    void take_sections(std::vector<std::vector<std::uint8_t>>& out);
};

// Program of the first PAT entry with program_number != 0: its PMT PID, or nullopt.
std::optional<std::uint16_t> pmt_pid_of_pat(const std::vector<std::uint8_t>& section);

// PIDs of all elementary streams of a PMT section with stream_type 0x1B (H.264) or 0x0F (AAC):
// the A/V PIDs of D-04. nullopt if the section is not a PMT.
std::optional<std::set<std::uint16_t>> av_pids_of_pmt(const std::vector<std::uint8_t>& section);

enum class PacketAction { Copied, Encrypted };

// Applies D-04 to one 188-byte packet in place: for a packet of an A/V PID with a payload, the
// largest multiple of 16 bytes from the payload start is encrypted with AES-128-CBC (IV = 16 zero
// bytes, a new cipher context per packet, no padding) and transport_scrambling_control is set to '10';
// everything else is left unchanged. Returns false only if OpenSSL fails.
bool package_packet(std::uint8_t* packet, const Key& key, const std::set<std::uint16_t>& av_pids,
                    PacketAction& action);

struct PidCount {
    std::uint64_t encrypted = 0;
    std::uint64_t clear = 0;
};

struct PackageResult {
    bool ok = false;
    std::string error;                       // message of ER-39 when !ok
    std::uint16_t pmt_pid = 0;
    std::set<std::uint16_t> av_pids;
    std::map<std::uint16_t, PidCount> counts;   // per PID of the file
    std::uint64_t bytes = 0;
};

// Packages `in` into `out` (Section 4.7.3). The output is written under a temporary name in the
// directory of `out` and renamed when it is complete, so that a failure leaves no output file (ER-39).
PackageResult package_file(const std::string& in, const std::string& out, const Key& key);

}  // namespace pkg
