// AL-OUT free functions (step 6): PCR and random_access_indicator extraction (TST-121, TST-124) and the
// playlist text (TST-123; example of Section 3.7).
#include <gtest/gtest.h>

#include <string>

#include "ffmpeg_fixtures.hpp"
#include "hls_playlist.hpp"
#include "ts_builder.hpp"
#include "ts_packet.hpp"

namespace {

using testutil::Bytes;

TEST(Tst121, PcrAndRaiFunctions) {
    // Key-frame packet of FFmpeg 6.1.1: PCR_base 63000, extension 0, random_access_indicator 1.
    Bytes ffmpeg = testutil::kFfmpegKeyFrameHead;
    ffmpeg.resize(188, 0x00);
    EXPECT_EQ(al::ts_pcr(ffmpeg.data()), std::optional<std::uint64_t>(63000ull * 300));
    EXPECT_TRUE(al::ts_random_access(ffmpeg.data()));

    // Builder packets (checked against FFmpeg in step 3a): base and extension, RAI without PCR.
    testutil::TsPacketSpec spec;
    spec.pid = 256;
    spec.pcr_base = 0x1FFFFFFFFull;   // largest 33-bit value
    spec.pcr_ext = 299;
    spec.payload = Bytes(100, 0x11);
    const Bytes max_pcr = testutil::make_ts_packet(spec);
    EXPECT_EQ(al::ts_pcr(max_pcr.data()), std::optional<std::uint64_t>(0x1FFFFFFFFull * 300 + 299));
    EXPECT_FALSE(al::ts_random_access(max_pcr.data()));

    testutil::TsPacketSpec rai;
    rai.pid = 256;
    rai.rai = true;
    rai.payload = Bytes(100, 0x22);
    const Bytes rai_only = testutil::make_ts_packet(rai);
    EXPECT_TRUE(al::ts_random_access(rai_only.data()));
    EXPECT_FALSE(al::ts_pcr(rai_only.data()).has_value());

    // No adaptation field, or an adaptation field of length 0: neither PCR nor RAI.
    testutil::TsPacketSpec plain;
    plain.pid = 256;
    plain.payload = Bytes(184, 0x40);   // 0x40 in the byte where the flags would be
    const Bytes no_af = testutil::make_ts_packet(plain);
    EXPECT_FALSE(al::ts_pcr(no_af.data()).has_value());
    EXPECT_FALSE(al::ts_random_access(no_af.data()));
    plain.payload = Bytes(183, 0x40);
    const Bytes empty_af = testutil::make_ts_packet(plain);
    EXPECT_EQ(empty_af[4], 0);
    EXPECT_FALSE(al::ts_random_access(empty_af.data()));
}

TEST(Tst123, PlaylistText) {
    // Example of Section 3.7 (one line each).
    const std::vector<al::PlaylistEntry> entries = {
        {41, 27000000, false}, {42, 27000000, false}, {43, 27000000, true}};
    EXPECT_EQ(al::playlist_text(entries, 0),
              "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:41\n"
              "#EXTINF:1.000,\nseg-41.ts\n#EXTINF:1.000,\nseg-42.ts\n"
              "#EXT-X-DISCONTINUITY\n#EXTINF:1.000,\nseg-43.ts\n");
    // #EXT-X-DISCONTINUITY-SEQUENCE only when needed.
    EXPECT_EQ(al::playlist_text({{44, 27000000, false}}, 1),
              "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:44\n"
              "#EXT-X-DISCONTINUITY-SEQUENCE:1\n#EXTINF:1.000,\nseg-44.ts\n");
}

TEST(Tst123, DurationWithThreeDecimals) {
    EXPECT_EQ(al::format_duration(27000000), "1.000");
    EXPECT_EQ(al::format_duration(25920000), "0.960");   // 24 frames of 40 ms
    EXPECT_EQ(al::format_duration(54000000), "2.000");
    EXPECT_EQ(al::format_duration(26999999), "1.000");   // rounded to the millisecond
    EXPECT_EQ(al::format_duration(27013500), "1.001");
    EXPECT_EQ(al::format_duration(0), "0.000");
    EXPECT_EQ(al::segment_name(17), "seg-17.ts");
}

}  // namespace
