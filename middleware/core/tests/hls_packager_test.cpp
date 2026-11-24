// AL-OUT module tests (step 6): TST-121 – TST-127. The synthetic stream has 25 frames per second with a
// key frame every 25 frames (closed GOP of 1 s, Section 2.2.1) and a PCR on every frame start (40 ms),
// so the expected cuts and durations follow from REQ-23 / REQ-24 directly. The content tests use
// content/svc-1.ts and ffprobe (Section 5.1.6) and fail while the content is missing.
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "content.hpp"
#include "fakes/call_log.hpp"
#include "fakes/fake_hls_store.hpp"
#include "fakes/fake_logger.hpp"
#include "hls_packager.hpp"
#include "psi_builder.hpp"
#include "ts_builder.hpp"
#include "ts_packet.hpp"

namespace {

using testutil::Bytes;

constexpr std::uint64_t kFrameTicks = 27000000 / 25;   // 40 ms

al::StreamDescription reference_stream() {
    al::StreamDescription d;
    d.program_number = 1;
    d.pmt_pid = 4096;
    d.pcr_pid = 256;
    d.video_pid = 256;
    d.video_stream_type = 0x1B;
    d.audio_pid = 257;
    d.audio_stream_type = 0x0F;
    return d;
}

// Synthetic stream of the reference PIDs (Section 5.2.1).
struct StreamGen {
    int frame = 0;                         // global frame number
    std::uint64_t pcr = 100 * 27000000ull;  // PCR of the next frame
    std::function<bool(int)> is_key = [](int f) { return f % 25 == 0; };
    std::uint8_t cc[4] = {0, 0, 0, 0};

    Bytes make(std::uint16_t pid, int cc_index, bool pusi, Bytes payload, std::optional<std::uint64_t> pcr_ticks = {},
               bool rai = false) {
        testutil::TsPacketSpec spec;
        spec.pid = pid;
        spec.pusi = pusi;
        spec.cc = cc[cc_index]++ & 0x0F;
        spec.rai = rai;
        if (pcr_ticks) {
            spec.pcr_base = *pcr_ticks / 300;
            spec.pcr_ext = static_cast<std::uint16_t>(*pcr_ticks % 300);
        }
        spec.payload = std::move(payload);
        return testutil::make_ts_packet(spec);
    }

    // Packets of `count` frames: PAT and PMT every second frame, the video frame start (PCR, RAI on
    // key frames), two video continuation packets and one audio packet.
    std::vector<Bytes> frames(int count) {
        std::vector<Bytes> out;
        for (int i = 0; i < count; ++i, ++frame, pcr += kFrameTicks) {
            if (frame % 2 == 0) {
                Bytes pat(1, 0x00);
                const Bytes s = testutil::make_pat({{1, 4096}});
                pat.insert(pat.end(), s.begin(), s.end());
                pat.resize(184, 0xFF);
                out.push_back(make(0, 0, true, pat));
                Bytes pmt(1, 0x00);
                const Bytes m = testutil::make_pmt(1, 256, {{0x1B, 256, {}}, {0x0F, 257, {}}});
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
        pcr += static_cast<std::uint64_t>(count) * kFrameTicks;
    }
};

std::vector<Bytes> split(const std::vector<std::uint8_t>& bytes) {
    std::vector<Bytes> packets;
    for (std::size_t i = 0; i + 188 <= bytes.size(); i += 188) packets.emplace_back(bytes.begin() + i, bytes.begin() + i + 188);
    return packets;
}

std::vector<std::string> lines_of(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::string> lines;
    std::istringstream in(std::string(bytes.begin(), bytes.end()));
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

int frame_of(const Bytes& video_start) {   // frame number written into the synthetic PES header
    const std::size_t o = al::ts_payload_offset(video_start.data());
    return (video_start[o + 4] << 8) | video_start[o + 5];
}

struct Harness {
    fakes::CallLog calls;
    fakes::FakeHlsStore store{&calls};
    fakes::FakeLogger logger;
    al::HlsPackager out{store, logger};
    int first_segment = 0;
    int errors = 0;
    std::vector<Bytes> fed;
    std::map<std::uint64_t, std::vector<Bytes>> written;   // every segment at the moment it was written
    std::vector<std::string> playlists;                     // every playlist written, in order
    std::function<void(std::uint64_t, const std::string&)> extra_hook;

    explicit Harness(bool write_enabled = true) {
        store.set_write_hook([this](std::uint64_t session, const std::string& name) {
            const std::vector<std::uint8_t> bytes = store.read(session, name);
            if (name == "index.m3u8") {
                playlists.emplace_back(bytes.begin(), bytes.end());
            } else if (name.rfind("seg-", 0) == 0) {
                written[std::stoull(name.substr(4))] = split(bytes);
            }
            if (extra_hook) extra_hook(session, name);
        });
        al::HlsPackager::Events events;
        events.on_first_segment = [this] { ++first_segment; };
        events.on_error = [this] { ++errors; };
        EXPECT_TRUE(out.open(1, 1, events));
        out.set_stream(reference_stream());
        out.set_write_enabled(write_enabled);
    }

    void feed(const std::vector<Bytes>& packets) {
        for (const Bytes& p : packets) {
            out.feed(p.data());
            fed.push_back(p);
        }
    }

    std::vector<std::string> playlist() const { return lines_of(store.read(1, "index.m3u8")); }
    std::vector<Bytes> segment(std::uint64_t k) const {
        auto it = written.find(k);
        return it == written.end() ? std::vector<Bytes>{} : it->second;
    }

    std::vector<std::string> extinf() const {
        std::vector<std::string> d;
        for (const std::string& l : playlist()) {
            if (l.rfind("#EXTINF:", 0) == 0) d.push_back(l.substr(8, l.size() - 9));
        }
        return d;
    }
};

// Checks REQ-23: PAT, PMT, then a key-frame start; returns the frame number of the key frame.
int check_segment_start(const std::vector<Bytes>& seg) {
    EXPECT_GE(seg.size(), 3u);
    if (seg.size() < 3) return -1;
    EXPECT_EQ(al::ts_pid(seg[0].data()), 0);
    EXPECT_EQ(al::ts_pid(seg[1].data()), 4096);
    EXPECT_EQ(al::ts_pid(seg[2].data()), 256);
    EXPECT_TRUE(al::ts_pusi(seg[2].data()));
    EXPECT_TRUE(al::ts_random_access(seg[2].data()));
    return frame_of(seg[2]);
}

// The packets of a segment after its PAT / PMT copies appear in the input in the same order and
// unchanged (REQ-22: received order; REQ-24: timestamps not rewritten).
void check_subsequence(const std::vector<Bytes>& seg, const std::vector<Bytes>& input) {
    std::size_t at = 0;
    for (std::size_t i = 2; i < seg.size(); ++i) {
        while (at < input.size() && input[at] != seg[i]) ++at;
        ASSERT_LT(at, input.size()) << "packet " << i << " of the segment is not in the input order";
        ++at;
    }
}

bool ffprobe_available() {
    if (std::system("ffprobe -version > /dev/null 2>&1") == 0) return true;
    ADD_FAILURE() << "ffprobe missing: install ffmpeg (Section 2.2.8)";
    return false;
}

// Output of ffprobe for one file (stdout and stderr), or nullopt if it exits with an error.
std::optional<std::string> ffprobe(const std::filesystem::path& file, const std::string& args) {
    const std::filesystem::path out = file.string() + ".probe";
    const std::string command = "ffprobe -v error " + args + " '" + file.string() + "' > '" + out.string() + "' 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream in(out);
    std::stringstream text;
    text << in.rdbuf();
    std::filesystem::remove(out);
    if (status != 0) return std::nullopt;
    return text.str();
}

// ---------------------------------------------------------------- TST-121, TST-122

TEST(Tst121, SegmentsStartWithPatPmtKeyFrame) {
    Harness h;
    StreamGen gen;
    h.feed(gen.frames(300));   // 12 s
    // Key frames at 0, 25, …, 275: 11 complete segments, the 12th still open.
    ASSERT_TRUE(h.store.session_exists(1));
    for (std::uint64_t k = 0; k < 11; ++k) {
        SCOPED_TRACE(k);
        const std::vector<Bytes> seg = h.segment(k);
        EXPECT_EQ(check_segment_start(seg), static_cast<int>(25 * k));
        check_subsequence(seg, h.fed);
    }
    EXPECT_EQ(h.written.size(), 11u);   // seg-11 is not complete yet
    const std::vector<std::string> durations = h.extinf();
    ASSERT_EQ(durations.size(), 6u);
    for (const std::string& d : durations) EXPECT_EQ(d, "1.000");
    EXPECT_EQ(h.first_segment, 1);
    EXPECT_TRUE(h.logger.contains("segment seg-0.ts duration=1.000 session=1 generation=1"));
}

TEST(Tst122, OnlyStreamPids) {
    Harness h;
    StreamGen gen;
    std::vector<Bytes> input = gen.frames(60);
    // A packet of another PID and a packet still scrambled ('10') are never output.
    input.insert(input.begin() + 40, gen.make(300, 0, true, Bytes(184, 0x01)));
    Bytes scrambled = gen.make(257, 3, true, Bytes(184, 0x02));
    scrambled[3] = static_cast<std::uint8_t>(scrambled[3] | 0x80);
    input.insert(input.begin() + 41, scrambled);
    h.feed(input);
    const std::set<std::uint16_t> allowed = {0, 4096, 256, 257};
    for (std::uint64_t k = 0; k < 2; ++k) {
        for (const Bytes& p : h.segment(k)) {
            EXPECT_EQ(allowed.count(al::ts_pid(p.data())), 1u);
            EXPECT_EQ((p[3] >> 6) & 0x3, 0);
        }
    }
}

// ---------------------------------------------------------------- TST-123

TEST(Tst123, PlaylistAndBound) {
    Harness h;
    int playlist_writes = 0;
    h.extra_hook = [&](std::uint64_t session, const std::string& name) {
        EXPECT_LE(h.store.segment_files(session), 8u) << "after " << name;   // B7
        if (name != "index.m3u8") return;
        ++playlist_writes;
        const std::vector<std::string> lines = lines_of(h.store.read(session, name));
        ASSERT_GE(lines.size(), 6u);
        EXPECT_EQ(lines[0], "#EXTM3U");
        EXPECT_EQ(lines[1], "#EXT-X-VERSION:3");
        EXPECT_EQ(lines[2], "#EXT-X-TARGETDURATION:2");
        ASSERT_EQ(lines[3].rfind("#EXT-X-MEDIA-SEQUENCE:", 0), 0u);
        const std::uint64_t first = std::stoull(lines[3].substr(22));
        std::vector<std::uint64_t> listed;
        for (std::size_t i = 4; i < lines.size(); ++i) {
            EXPECT_NE(lines[i], "#EXT-X-ENDLIST");
            if (lines[i].rfind("#EXTINF:", 0) == 0) {
                EXPECT_EQ(lines[i].size(), std::string("#EXTINF:1.000,").size()) << lines[i];   // 3 decimals
            } else if (lines[i].rfind("seg-", 0) == 0) {
                listed.push_back(std::stoull(lines[i].substr(4)));
                // Listed only when complete: the file exists with whole packets.
                const std::vector<std::uint8_t> file = h.store.read(session, lines[i]);
                EXPECT_FALSE(file.empty()) << lines[i];
                EXPECT_EQ(file.size() % 188, 0u) << lines[i];
            }
        }
        ASSERT_FALSE(listed.empty());
        EXPECT_LE(listed.size(), 6u);
        EXPECT_EQ(listed.front(), first);
        if (first >= 3) {
            EXPECT_FALSE(std::filesystem::exists(h.store.dir(session) / al::segment_name(first - 3)));
        }
    };
    StreamGen gen;
    h.feed(gen.frames(500));   // 20 s: 19 complete segments
    EXPECT_EQ(playlist_writes, 19);
    EXPECT_EQ(h.store.max_segment_files(1), 8u);
    EXPECT_EQ(h.store.segment_files(1), 8u);   // seg-11 … seg-18
    // Remove before write (Section 4.2.4): remove_file of seg-(k-8) right before write_file of seg-k.
    const std::vector<std::string> entries = h.calls.entries();
    for (std::uint64_t k = 8; k < 19; ++k) {
        const std::string write = "write_file 1 " + al::segment_name(k);
        std::size_t i = 0;
        while (i < entries.size() && entries[i].rfind(write, 0) != 0) ++i;
        ASSERT_LT(i, entries.size()) << write;
        ASSERT_GT(i, 0u);
        EXPECT_EQ(entries[i - 1], "remove_file 1 " + al::segment_name(k - 8));
    }
}

// ---------------------------------------------------------------- TST-124

TEST(Tst124, LoopBoundaryAndGap) {
    for (bool gap : {false, true}) {
        SCOPED_TRACE(gap ? "gap of 2 s" : "loop boundary");
        Harness h;
        const std::vector<std::string>& playlists = h.playlists;
        StreamGen gen;
        gen.pcr = 590 * 27000000ull;
        h.feed(gen.frames(125));   // 5 s: key frames 0 … 100
        if (gap) {
            gen.skip(50);          // 2 s of content removed: the PCR jumps forward by 2.04 s
        } else {
            gen.pcr = 0;           // loop boundary of D-08: the PCR goes back by 600 s
        }
        h.feed(gen.frames(225));   // 9 s after the boundary / the gap

        // seg-4 (frames 100 – 124) is closed at the boundary, 24 frames long.
        EXPECT_EQ(check_segment_start(h.segment(4)), 100);
        EXPECT_NE(playlists[4].find("#EXTINF:0.960,\nseg-4.ts\n"), std::string::npos);
        // seg-5 starts at the first key frame after the boundary and is marked.
        EXPECT_EQ(check_segment_start(h.segment(5)), gap ? 175 : 125);
        EXPECT_NE(playlists[5].find("#EXT-X-DISCONTINUITY\n#EXTINF:1.000,\nseg-5.ts\n"), std::string::npos);
        EXPECT_EQ(playlists[5].find("#EXT-X-DISCONTINUITY-SEQUENCE"), std::string::npos);
        // While seg-5 is listed (seg-5 … seg-10 as the last window) the tag stays.
        EXPECT_NE(playlists[10].find("#EXT-X-DISCONTINUITY\n"), std::string::npos);
        // When seg-5 leaves the playlist: #EXT-X-DISCONTINUITY-SEQUENCE:1 and no tag.
        ASSERT_GE(playlists.size(), 12u);
        EXPECT_NE(playlists[11].find("#EXT-X-MEDIA-SEQUENCE:6\n#EXT-X-DISCONTINUITY-SEQUENCE:1\n"), std::string::npos)
            << playlists[11];
        EXPECT_EQ(playlists[11].find("#EXT-X-DISCONTINUITY\n"), std::string::npos);
        // Timestamps are not rewritten: every segment consists of input packets in input order.
        for (std::uint64_t k = 6; k < 12; ++k) check_subsequence(h.segment(k), h.fed);
        EXPECT_TRUE(h.logger.contains("#EXT-X-DISCONTINUITY before seg-5.ts"));
    }
}

// ---------------------------------------------------------------- TST-125

TEST(Tst125, HeldUntilWriteEnabled) {
    {
        Harness h(false);
        StreamGen gen;
        h.feed(gen.frames(125));   // 5 s: 4 complete segments
        EXPECT_TRUE(h.store.files(1).empty());   // no segment and no playlist
        EXPECT_EQ(h.out.held_segments(), 4u);
        EXPECT_EQ(h.first_segment, 0);
        h.out.set_write_enabled(true);
        EXPECT_EQ(h.out.held_segments(), 0u);
        for (std::uint64_t k = 0; k < 4; ++k) EXPECT_EQ(check_segment_start(h.segment(k)), static_cast<int>(25 * k));
        EXPECT_EQ(h.first_segment, 1);
        EXPECT_EQ(h.extinf().size(), 4u);
    }
    {
        Harness h(false);   // B6: a fifth held segment drops the oldest one
        StreamGen gen;
        h.feed(gen.frames(150));
        EXPECT_EQ(h.out.held_segments(), 4u);
        EXPECT_EQ(h.logger.count_containing("held segment dropped"), 1u);
        h.out.set_write_enabled(true);
        EXPECT_EQ(check_segment_start(h.segment(0)), 25);
    }
}

// ---------------------------------------------------------------- TST-126

TEST(Tst126, NoKeyFrameWithin2s) {
    Harness h;
    StreamGen gen;
    gen.is_key = [](int f) { return f % 25 == 0 && (f < 25 || f > 87); };   // key frames 1 s – 3.5 s cleared
    h.feed(gen.frames(151));
    EXPECT_EQ(h.logger.count_containing("segment discarded: no key-frame start within 2 s of content"), 1u);
    EXPECT_EQ(check_segment_start(h.segment(0)), 100);   // the next key-frame start
    EXPECT_EQ(check_segment_start(h.segment(1)), 125);
    for (const std::string& d : h.extinf()) EXPECT_LE(std::stod(d), 2.0);
}

// ---------------------------------------------------------------- TST-127

TEST(Tst127, WriteFailure) {
    Harness h;
    h.store.fail_writes_from(3);   // seg-0 and the playlist succeed, seg-1 fails
    StreamGen gen;
    h.feed(gen.frames(60));
    EXPECT_EQ(h.first_segment, 1);
    EXPECT_EQ(h.errors, 1);
    h.feed(gen.frames(100));
    EXPECT_EQ(h.errors, 1);                 // once
    EXPECT_EQ(h.store.write_calls(), 3u);   // nothing written afterwards
    EXPECT_TRUE(h.logger.contains("cannot write seg-1.ts"));

    fakes::FakeHlsStore failing;
    failing.fail_open(true);
    fakes::FakeLogger logger;
    al::HlsPackager out(failing, logger);
    EXPECT_FALSE(out.open(2, 1, {}));       // open_session false: AL-SES makes it EV-OUT-ERR
}

// ---------------------------------------------------------------- content: TST-121, TST-124

// Feeds packets of the four PIDs of the reference set (what the TS filters of REQ-19 deliver).
void feed_file(Harness& h, const std::string& path, std::size_t first, std::size_t count) {
    std::ifstream file(path, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(first * 188));
    Bytes p(188);
    for (std::size_t i = 0; i < count && file.read(reinterpret_cast<char*>(p.data()), 188); ++i) {
        const std::uint16_t pid = al::ts_pid(p.data());
        if (pid == 0 || pid == 4096 || pid == 256 || pid == 257) h.out.feed(p.data());
    }
}

TEST(Tst121, ContentFirst12s) {
    const std::string path = testutil::content_path("svc-1.ts");
    if (!testutil::content_exists(path) || !ffprobe_available()) return;
    Harness h;
    feed_file(h, path, 0, testutil::packets_for_seconds(12));
    const std::vector<std::string> durations = h.extinf();
    ASSERT_FALSE(durations.empty());
    for (const std::string& d : durations) {
        EXPECT_GE(std::stod(d), 1.0);
        EXPECT_LE(std::stod(d), 2.0);
    }
    for (const std::string& name : h.store.files(1)) {
        if (name.rfind("seg-", 0) != 0) continue;
        SCOPED_TRACE(name);
        check_segment_start(split(h.store.read(1, name)));
        // ffprobe reads every segment on its own; its first video frame is a key frame.
        const auto frames = ffprobe(h.store.dir(1) / name,
                                    "-select_streams v:0 -show_entries frame=key_frame -of csv=p=0");
        ASSERT_TRUE(frames.has_value());
        EXPECT_EQ(frames->substr(0, 1), "1") << *frames;
    }
}

TEST(Tst124, ContentLoop) {
    const std::string path = testutil::content_path("svc-1.ts");
    if (!testutil::content_exists(path) || !ffprobe_available()) return;
    Harness h;
    const std::size_t total = static_cast<std::size_t>(std::filesystem::file_size(path) / 188);
    const std::size_t last5 = testutil::packets_for_seconds(5);
    feed_file(h, path, total - last5, last5);                 // last 5 s of the file
    feed_file(h, path, 0, testutil::packets_for_seconds(8));  // then its first 8 s (D-08)
    // The marked segment is listed (#EXT-X-DISCONTINUITY) or has left (#EXT-X-DISCONTINUITY-SEQUENCE:1).
    std::string text;
    for (const std::string& l : h.playlist()) text += l + "\n";
    EXPECT_NE(text.find("#EXT-X-DISCONTINUITY"), std::string::npos) << text;
    EXPECT_TRUE(h.logger.contains("#EXT-X-DISCONTINUITY before"));
    for (const std::string& name : h.store.files(1)) {
        if (name.rfind("seg-", 0) != 0) continue;
        SCOPED_TRACE(name);
        EXPECT_TRUE(ffprobe(h.store.dir(1) / name, "-show_programs -show_streams").has_value());
    }
}

}  // namespace
