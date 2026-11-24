#include "hls_playlist.hpp"

#include <cstdio>

namespace al {

std::string format_duration(std::uint64_t ticks) {
    const std::uint64_t ms = (ticks + 13500) / 27000;   // 27 000 ticks per millisecond, rounded
    char text[32];
    std::snprintf(text, sizeof text, "%llu.%03llu", static_cast<unsigned long long>(ms / 1000),
                  static_cast<unsigned long long>(ms % 1000));
    return text;
}

std::string segment_name(std::uint64_t sequence) { return "seg-" + std::to_string(sequence) + ".ts"; }

std::string playlist_text(const std::vector<PlaylistEntry>& entries, std::uint64_t discontinuity_sequence) {
    std::string text = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n";
    text += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(entries.empty() ? 0 : entries.front().sequence) + "\n";
    if (discontinuity_sequence != 0) {
        text += "#EXT-X-DISCONTINUITY-SEQUENCE:" + std::to_string(discontinuity_sequence) + "\n";
    }
    for (const PlaylistEntry& e : entries) {
        if (e.discontinuity) text += "#EXT-X-DISCONTINUITY\n";
        text += "#EXTINF:" + format_duration(e.duration_ticks) + ",\n";
        text += segment_name(e.sequence) + "\n";
    }
    return text;
}

}  // namespace al
