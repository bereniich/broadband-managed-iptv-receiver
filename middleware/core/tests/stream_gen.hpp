// Test helper: a synthetic SPTS with the PIDs of the reference set (Section 5.2.1): 25 frames per
// second, a key frame (random_access_indicator) every 25 frames (closed GOP of 1 s, Section 2.2.1),
// a PCR on every frame start (40 ms), PAT and PMT every second frame. With `key` set, the video and
// audio packets are encrypted by the reference implementation of D-04 of the test code (packets with
// fewer than 16 payload bytes stay clear, Section 4.7.3). Not part of the code under test.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "fakes/reference_d04.hpp"
#include "psi_builder.hpp"
#include "ts_builder.hpp"

namespace testutil {

constexpr std::uint64_t kFrameTicks = 27000000 / 25;   // 40 ms in 27 MHz

struct StreamGen {
    int frame = 0;                         // global frame number
    std::uint64_t pcr = 100 * 27000000ull;  // PCR of the next frame
    std::function<bool(int)> is_key = [](int f) { return f % 25 == 0; };
    std::uint8_t cc[4] = {0, 0, 0, 0};
    std::uint64_t frame_ticks = kFrameTicks;   // PCR step per frame (exactly 40 ms by default)
    std::optional<fakes::AesKey> key;          // D-04 encryption of PIDs 256 and 257 (protected service)

    Bytes make(std::uint16_t pid, int cc_index, bool pusi, Bytes payload, std::optional<std::uint64_t> pcr_ticks = {},
               bool rai = false) {
        TsPacketSpec spec;
        spec.pid = pid;
        spec.pusi = pusi;
        spec.cc = cc[cc_index]++ & 0x0F;
        spec.rai = rai;
        if (pcr_ticks) {
            spec.pcr_base = *pcr_ticks / 300;
            spec.pcr_ext = static_cast<std::uint16_t>(*pcr_ticks % 300);
        }
        spec.payload = std::move(payload);
        Bytes packet = make_ts_packet(spec);
        if (key && (pid == 256 || pid == 257)) fakes::d04_encrypt_packet(*key, packet.data());
        return packet;
    }

    // Packets of `count` frames: PAT and PMT every second frame, the video frame start (PCR, RAI on
    // key frames), two video continuation packets and one audio packet.
    std::vector<Bytes> frames(int count) {
        std::vector<Bytes> out;
        for (int i = 0; i < count; ++i, ++frame, pcr += frame_ticks) {
            if (frame % 2 == 0) {
                Bytes pat(1, 0x00);
                const Bytes s = make_pat({{1, 4096}});
                pat.insert(pat.end(), s.begin(), s.end());
                pat.resize(184, 0xFF);
                out.push_back(make(0, 0, true, pat));
                Bytes pmt(1, 0x00);
                const Bytes m = make_pmt(1, 256, {{0x1B, 256, {}}, {0x0F, 257, {}}});
                pmt.insert(pmt.end(), m.begin(), m.end());
                pmt.resize(184, 0xFF);
                out.push_back(make(4096, 1, true, pmt));
            }
            Bytes pes = {0x00, 0x00, 0x01, 0xE0, static_cast<std::uint8_t>(frame >> 8),
                         static_cast<std::uint8_t>(frame)};
            pes.resize(176, 0x33);
            out.push_back(make(256, 2, true, pes, pcr, is_key(frame)));
            out.push_back(make(256, 2, false, Bytes(184, static_cast<std::uint8_t>(frame))));
            out.push_back(make(256, 2, false, Bytes(184, static_cast<std::uint8_t>(frame + 1))));
            out.push_back(make(257, 3, true, Bytes(184, 0xAA)));
        }
        return out;
    }

    void skip(int count) {   // frames removed from the input (a gap of content)
        frame += count;
        pcr += static_cast<std::uint64_t>(count) * frame_ticks;
    }
};

}  // namespace testutil
