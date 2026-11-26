#include "ts_builder.hpp"

#include <algorithm>
#include <stdexcept>

namespace testutil {

Bytes make_ts_packet(const TsPacketSpec& spec) {
    constexpr std::size_t kMaxPayload = kTsPacketSize - 4;
    if (spec.payload.size() > kMaxPayload) {
        throw std::invalid_argument("make_ts_packet: payload larger than 184 bytes");
    }
    const bool has_flags = spec.rai || spec.pcr_base.has_value();
    const bool has_af = spec.adaptation || has_flags || spec.payload.size() < kMaxPayload;
    const std::size_t af_total = kMaxPayload - spec.payload.size();   // including the length byte
    if (has_af && af_total < (has_flags ? 2u + (spec.pcr_base ? 6u : 0u) : 1u)) {
        throw std::invalid_argument("make_ts_packet: adaptation field does not fit");
    }

    Bytes p;
    p.reserve(kTsPacketSize);
    p.push_back(0x47);
    p.push_back(static_cast<std::uint8_t>((spec.pusi ? 0x40 : 0x00) | ((spec.pid >> 8) & 0x1F)));
    p.push_back(static_cast<std::uint8_t>(spec.pid & 0xFF));
    const std::uint8_t afc = has_af ? (spec.payload.empty() ? 0x2 : 0x3) : 0x1;
    p.push_back(static_cast<std::uint8_t>(((spec.scrambling & 0x3) << 6) | (afc << 4) | (spec.cc & 0x0F)));

    if (has_af) {
        p.push_back(static_cast<std::uint8_t>(af_total - 1));   // adaptation_field_length
        if (af_total > 1) {
            p.push_back(static_cast<std::uint8_t>((spec.rai ? 0x40 : 0x00) | (spec.pcr_base ? 0x10 : 0x00)));
            if (spec.pcr_base) {
                const std::uint64_t base = *spec.pcr_base & 0x1FFFFFFFFull;
                p.push_back(static_cast<std::uint8_t>(base >> 25));
                p.push_back(static_cast<std::uint8_t>(base >> 17));
                p.push_back(static_cast<std::uint8_t>(base >> 9));
                p.push_back(static_cast<std::uint8_t>(base >> 1));
                // last bit of the base, 6 reserved bits '111111', high bit of the extension
                p.push_back(static_cast<std::uint8_t>(((base & 1) << 7) | 0x7E | ((spec.pcr_ext >> 8) & 1)));
                p.push_back(static_cast<std::uint8_t>(spec.pcr_ext & 0xFF));
            }
            while (p.size() < 4 + af_total) {
                p.push_back(0xFF);   // stuffing
            }
        }
    }
    p.insert(p.end(), spec.payload.begin(), spec.payload.end());
    return p;
}

std::vector<Bytes> packetize_section(std::uint16_t pid, const Bytes& section, std::uint8_t cc) {
    Bytes data;
    data.push_back(0x00);   // pointer_field
    data.insert(data.end(), section.begin(), section.end());

    std::vector<Bytes> packets;
    std::size_t offset = 0;
    bool first = true;
    while (offset < data.size()) {
        const std::size_t n = std::min<std::size_t>(184, data.size() - offset);
        Bytes payload(data.begin() + static_cast<std::ptrdiff_t>(offset),
                      data.begin() + static_cast<std::ptrdiff_t>(offset + n));
        payload.resize(184, 0xFF);   // PSI stuffing after the end of the section
        TsPacketSpec spec;
        spec.pid = pid;
        spec.pusi = first;
        spec.cc = static_cast<std::uint8_t>(cc & 0x0F);
        spec.payload = std::move(payload);
        packets.push_back(make_ts_packet(spec));
        offset += n;
        first = false;
        ++cc;
    }
    return packets;
}

Bytes join(const std::vector<Bytes>& packets) {
    Bytes out;
    for (const Bytes& p : packets) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

}  // namespace testutil
