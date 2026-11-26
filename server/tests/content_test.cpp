// Unit tests of SRV-GEN (content/generate.sh): TST-301, TST-302, TST-303 (SPECIFICATION.md, Section 5.3.4).
//
// The expected values come from the specification (Sections 2.2.1, 4.7.2, 5.2.1) and are
// checked with ffprobe / ffmpeg and with a TS reader written here, independently of the
// generator. The tests need the content files; without them they fail (Section 5.3.1).
#include <gtest/gtest.h>
#include <json.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const fs::path kContentDir = fs::path(M15_REPO_DIR) / "content";
const fs::path kOutputDir = fs::path(M15_TEST_OUTPUT_DIR);
constexpr double kMuxRate = 3'500'000.0;  // bit/s, Section 4.7.2
constexpr std::size_t kTs = 188;

// Runs a shell command and returns its standard output; `ok` is false on a non-zero exit status.
std::string run(const std::string& cmd, bool& ok) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        ok = false;
        return out;
    }
    char buf[65536];
    std::size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    ok = pclose(p) == 0;
    return out;
}

std::string quoted(const fs::path& p) { return "'" + p.string() + "'"; }

// Section 5.3.1: a test that needs content fails (never skips) when the file is missing.
::testing::AssertionResult content_present(const fs::path& p) {
    if (fs::exists(p)) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "content missing: run content/generate.sh (" << p.string() << ")";
}

struct Expected {
    const char* file;  // relative to content/
    int program;
    int pmt_pid;
    int video_pid;  // 0: no video stream
    int audio_pid;
    int pcr_pid;
};

// Section 5.2.1. The encrypted files (*.enc.ts) are not probed here: their payload, including
// the PES headers and the H.264 parameter sets, is encrypted (D-04), so ffprobe cannot read
// profile, size or timestamps of them. Their clear inputs svc-3.ts, svc-4.ts and
// test/svc-4.altpid.ts are checked instead (open point reported to the team for version 1.2).
const Expected kFiles[] = {
    {"svc-1.ts", 1, 4096, 256, 257, 256},
    {"svc-2.ts", 1, 4096, 256, 257, 256},
    {"svc-3.ts", 1, 4096, 256, 257, 256},
    {"svc-4.ts", 1, 4096, 256, 257, 256},
    {"test/novideo.ts", 1, 4096, 0, 256, 256},
    {"test/svc-2.altpid.ts", 2, 768, 1025, 1026, 1025},
    {"test/svc-4.altpid.ts", 2, 768, 1025, 1026, 1025},
};

int pid_of(const json& stream) { return std::stoi(stream.at("id").get<std::string>(), nullptr, 16); }

}  // namespace

// TST-301 (REQ-62): every property of the "Expected properties" table of Section 5.2.1.
TEST(Tst301, FfprobePropertiesOfEveryContentFile) {
    for (const auto& e : kFiles) {
        SCOPED_TRACE(e.file);
        const fs::path file = kContentDir / e.file;
        ASSERT_TRUE(content_present(file));
        bool ok = false;
        const std::string out =
            run("ffprobe -v error -show_programs -show_streams -show_format -of json " + quoted(file), ok);
        ASSERT_TRUE(ok) << "ffprobe failed";
        const json j = json::parse(out);

        ASSERT_EQ(j.at("programs").size(), 1u);
        const json& prog = j["programs"][0];
        EXPECT_EQ(prog.at("program_num").get<int>(), e.program);
        EXPECT_EQ(prog.at("pmt_pid").get<int>(), e.pmt_pid);
        EXPECT_EQ(prog.at("pcr_pid").get<int>(), e.pcr_pid);

        int videos = 0, audios = 0;
        for (const json& s : j.at("streams")) {
            const std::string type = s.at("codec_type");
            if (type == "video") {
                ++videos;
                EXPECT_EQ(pid_of(s), e.video_pid);
                EXPECT_EQ(s.at("codec_name"), "h264");
                EXPECT_EQ(s.at("profile"), "High");
                EXPECT_EQ(s.at("width").get<int>(), 1280);
                EXPECT_EQ(s.at("height").get<int>(), 720);
                EXPECT_EQ(s.at("r_frame_rate"), "25/1");
            } else if (type == "audio") {
                ++audios;
                EXPECT_EQ(pid_of(s), e.audio_pid);
                EXPECT_EQ(s.at("codec_name"), "aac");
                EXPECT_EQ(s.at("profile"), "LC");
                EXPECT_EQ(s.at("sample_rate"), "48000");
                EXPECT_EQ(s.at("channels").get<int>(), 2);
            }
        }
        EXPECT_EQ(videos, e.video_pid ? 1 : 0);
        EXPECT_EQ(audios, 1);

        const double duration = std::stod(j.at("format").at("duration").get<std::string>());
        const double bitrate = std::stod(j.at("format").at("bit_rate").get<std::string>());
        EXPECT_NEAR(duration, 600.0, 0.1);
        EXPECT_NEAR(bitrate, kMuxRate, kMuxRate * 0.01);
    }
}

namespace {

// Minimal TS reader of the test (not the code under test, Section 5.3.1 "Test helpers").
struct TsScan {
    int pmt_pid = -1;
    int video_pid = -1;
    double max_pat_gap_s = 0;
    double max_pmt_gap_s = 0;
    int pat_count = 0;
    int pmt_count = 0;
    int idr_pes = 0;              // video PES that contain an IDR NAL unit (type 5)
    int idr_pes_without_rai = 0;  // ... whose first TS packet has no random_access_indicator
    int sync_errors = 0;
};

// Returns true if the PES payload (after the PES header) contains a NAL unit of type 5.
bool pes_has_idr(const std::vector<std::uint8_t>& pes) {
    if (pes.size() < 9 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1) return false;
    const std::size_t start = 9 + pes[8];
    for (std::size_t i = start; i + 3 < pes.size(); ++i) {
        if (pes[i] == 0 && pes[i + 1] == 0 && pes[i + 2] == 1 && (pes[i + 3] & 0x1F) == 5) return true;
    }
    return false;
}

TsScan scan_ts(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TsScan r;
    long last_pat = -1, last_pmt = -1;
    std::vector<std::uint8_t> pes;
    bool pes_rai = false, have_pes = false;
    auto finish_pes = [&] {
        if (have_pes && pes_has_idr(pes)) {
            ++r.idr_pes;
            if (!pes_rai) ++r.idr_pes_without_rai;
        }
    };
    const double packet_s = kTs * 8 / kMuxRate;
    const long n = static_cast<long>(data.size() / kTs);
    for (long i = 0; i < n; ++i) {
        const std::uint8_t* p = &data[static_cast<std::size_t>(i) * kTs];
        if (p[0] != 0x47) {
            ++r.sync_errors;
            continue;
        }
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        const bool pusi = p[1] & 0x40;
        const int afc = (p[3] >> 4) & 3;
        std::size_t off = 4;
        bool rai = false;
        if (afc & 2) {
            if (p[4] > 0) rai = p[5] & 0x40;
            off = 5 + p[4];
        }
        if (!(afc & 1) || off >= kTs) continue;  // no payload

        if (pid == 0) {
            if (last_pat >= 0) r.max_pat_gap_s = std::max(r.max_pat_gap_s, (i - last_pat) * packet_s);
            last_pat = i;
            ++r.pat_count;
            if (pusi && r.pmt_pid < 0) {
                const std::uint8_t* s = p + off + 1 + p[off];  // skip pointer_field
                // program loop starts at byte 8; first program with program_number != 0
                for (const std::uint8_t* q = s + 8; q + 4 <= s + 3 + (((s[1] & 0x0F) << 8) | s[2]) - 4; q += 4) {
                    if (((q[0] << 8) | q[1]) != 0) {
                        r.pmt_pid = ((q[2] & 0x1F) << 8) | q[3];
                        break;
                    }
                }
            }
        } else if (pid == r.pmt_pid) {
            if (last_pmt >= 0) r.max_pmt_gap_s = std::max(r.max_pmt_gap_s, (i - last_pmt) * packet_s);
            last_pmt = i;
            ++r.pmt_count;
            if (pusi && r.video_pid < 0) {
                const std::uint8_t* s = p + off + 1 + p[off];
                const int section_length = ((s[1] & 0x0F) << 8) | s[2];
                const int info_len = ((s[10] & 0x0F) << 8) | s[11];
                const std::uint8_t* end = s + 3 + section_length - 4;
                for (const std::uint8_t* q = s + 12 + info_len; q + 5 <= end;) {
                    const int es_info = ((q[3] & 0x0F) << 8) | q[4];
                    if (q[0] == 0x1B) {
                        r.video_pid = ((q[1] & 0x1F) << 8) | q[2];
                        break;
                    }
                    q += 5 + es_info;
                }
            }
        } else if (pid == r.video_pid) {
            if (pusi) {
                finish_pes();
                pes.assign(p + off, p + kTs);
                pes_rai = rai;
                have_pes = true;
            } else if (have_pes) {
                pes.insert(pes.end(), p + off, p + kTs);
            }
        }
    }
    finish_pes();
    return r;
}

}  // namespace

// TST-302 (REQ-62, Section 2.2.1): PAT/PMT distance, random_access_indicator on every IDR start,
// an I frame every 25 frames, every GOP closed (every key frame is an IDR frame).
TEST(Tst302, PsiPeriodRandomAccessAndClosedGop) {
    for (int n = 1; n <= 4; ++n) {
        const fs::path file = kContentDir / ("svc-" + std::to_string(n) + ".ts");
        SCOPED_TRACE(file.string());
        ASSERT_TRUE(content_present(file));

        const TsScan r = scan_ts(file);
        EXPECT_EQ(r.sync_errors, 0);
        ASSERT_EQ(r.pmt_pid, 4096);
        ASSERT_EQ(r.video_pid, 256);
        EXPECT_GT(r.pat_count, 6000);  // at least 10 per second over 600 s
        EXPECT_GT(r.pmt_count, 6000);
        EXPECT_LE(r.max_pat_gap_s, 0.1);
        EXPECT_LE(r.max_pmt_gap_s, 0.1);
        EXPECT_EQ(r.idr_pes_without_rai, 0);

        bool ok = false;
        const std::string frames =
            run("ffprobe -v error -select_streams v:0 -show_entries frame=pict_type -of csv=p=0 " + quoted(file), ok);
        ASSERT_TRUE(ok) << "ffprobe -show_frames failed";
        int index = 0, i_frames = 0, misplaced = 0;
        std::size_t pos = 0;
        while (pos < frames.size()) {
            const std::size_t eol = frames.find('\n', pos);
            const std::string type = frames.substr(pos, (eol == std::string::npos ? frames.size() : eol) - pos);
            pos = eol == std::string::npos ? frames.size() : eol + 1;
            if (type.empty()) continue;
            const bool is_i = type[0] == 'I';
            if (is_i) ++i_frames;
            if (is_i != (index % 25 == 0)) ++misplaced;
            ++index;
        }
        EXPECT_EQ(index, 15000);  // 600 s × 25 fps
        EXPECT_EQ(misplaced, 0) << "I frames must be exactly every 25 frames";
        EXPECT_EQ(i_frames, 600);
        EXPECT_EQ(r.idr_pes, i_frames) << "every I frame must be an IDR frame (closed GOP)";
    }
}

// TST-303 (REQ-62, NFR-04): frames at 0, 123, 299, 300 and 599 s as PNG. The burned-in service
// name, program title and time code (00:00, 02:03, 04:59, 05:00, 09:59) are checked by
// inspection of the PNG files (evidence PHOTO, Section 5.1.6); this test produces them.
TEST(Tst303, ExtractFramesForInspection) {
    const fs::path dir = kOutputDir / "TST-303";
    fs::create_directories(dir);
    for (int n = 1; n <= 4; ++n) {
        const fs::path file = kContentDir / ("svc-" + std::to_string(n) + ".ts");
        SCOPED_TRACE(file.string());
        ASSERT_TRUE(content_present(file));
        for (int t : {0, 123, 299, 300, 599}) {
            const fs::path png = dir / ("svc-" + std::to_string(n) + "-" + std::to_string(t) + "s.png");
            fs::remove(png);
            bool ok = false;
            run("ffmpeg -v error -nostdin -y -ss " + std::to_string(t) + " -i " + quoted(file) +
                    " -frames:v 1 " + quoted(png),
                ok);
            EXPECT_TRUE(ok) << "ffmpeg failed at " << t << " s";
            EXPECT_TRUE(fs::exists(png) && fs::file_size(png) > 0) << png.string();
        }
    }
    std::cout << "TST-303: inspect the PNG files in " << dir.string() << '\n';
}
