// Playlist text of IF-INT-02 (Section 3.7; RFC 8216): a live media playlist, version 3, target
// duration 2, the listed segments with their durations in 3 decimals, no #EXT-X-ENDLIST.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace al {

struct PlaylistEntry {
    std::uint64_t sequence = 0;        // seg-<sequence>.ts
    std::uint64_t duration_ticks = 0;  // 27 MHz (PCR difference)
    bool discontinuity = false;        // preceded by #EXT-X-DISCONTINUITY (REQ-24)
};

// Duration in seconds with 3 decimals, rounded to the millisecond: 27 000 000 ticks -> "1.000".
std::string format_duration(std::uint64_t ticks);

// `entries` must not be empty; #EXT-X-MEDIA-SEQUENCE is the sequence of the first entry;
// #EXT-X-DISCONTINUITY-SEQUENCE is written only when it is not 0 ("when needed", Section 3.7).
std::string playlist_text(const std::vector<PlaylistEntry>& entries, std::uint64_t discontinuity_sequence);

std::string segment_name(std::uint64_t sequence);   // "seg-<sequence>.ts"

}  // namespace al
