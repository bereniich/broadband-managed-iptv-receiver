// Unit tests of PAL-FS (Section 5.3.3): TST-224 (REQ-23 atomic files, HLR-17, B7). The step
// "Platform::start()" runs here as HlsStore::reset(), which Platform::start() calls.
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "pal_hls_store.hpp"
#include "test_util.hpp"

namespace {

namespace fs = std::filesystem;
using pal::HlsStore;
using pal::LogLevel;
using testutil::RecordingLogger;
using testutil::TempDir;

constexpr std::size_t kSegmentSize = 1000000;  // "1 MB" of TST-224

std::vector<std::string> names_in(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        names.push_back(it->path().filename().string());
    }
    return names;
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

// Steps 1 – 5 of TST-224.
TEST(Tst224, StartEmptiesHlsDirWritesAreAtomicAndCloseRemovesTheSession) {
    TempDir tmp;
    const fs::path hls = tmp.path() / "m15-hls";
    write_text(hls / "1" / "seg-7.ts", "old segment");
    write_text(hls / "1" / "index.m3u8", "#EXTM3U");
    write_text(hls / "3" / "deep" / "x.ts", "old");
    write_text(hls / "stray.txt", "old");
    RecordingLogger log;
    HlsStore store(hls.string(), &log);

    // 1: hls_dir empty.
    ASSERT_TRUE(store.reset());
    EXPECT_TRUE(fs::is_directory(hls));
    EXPECT_TRUE(names_in(hls).empty());

    // 2: the session directory exists and is empty.
    ASSERT_TRUE(store.open_session(1));
    const fs::path session = hls / "1";
    EXPECT_TRUE(fs::is_directory(session));
    EXPECT_TRUE(names_in(session).empty());

    // 3: a reader sees only a missing file or the complete file of one write; a listing never shows
    //    another name that matches "seg-*.ts" (the temporary name starts with a dot).
    std::atomic<bool> writing{true};
    std::atomic<int> missing{0}, complete{0}, broken{0}, foreign_segments{0};
    std::thread reader([&] {
        std::vector<char> data;
        while (writing) {
            std::ifstream in(session / "seg-0.ts", std::ios::binary);
            if (!in) {
                ++missing;
            } else {
                data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                bool uniform = data.size() == kSegmentSize;
                for (std::size_t i = 1; uniform && i < data.size(); i += 4093) uniform = data[i] == data[0];
                if (uniform) uniform = data.back() == data[0];
                uniform ? ++complete : ++broken;
            }
            for (const std::string& name : names_in(session)) {
                if (name.rfind("seg-", 0) == 0 && name != "seg-0.ts") ++foreign_segments;
            }
        }
    });
    std::vector<std::uint8_t> segment(kSegmentSize);
    int failed_writes = 0;
    for (int i = 0; i < 100; ++i) {
        std::fill(segment.begin(), segment.end(), static_cast<std::uint8_t>(i));
        if (!store.write_file(1, "seg-0.ts", segment.data(), segment.size())) ++failed_writes;
    }
    writing = false;
    reader.join();
    ASSERT_EQ(failed_writes, 0);
    EXPECT_EQ(broken.load(), 0);
    EXPECT_EQ(foreign_segments.load(), 0);
    EXPECT_GT(complete.load(), 0);
    EXPECT_EQ(names_in(session), std::vector<std::string>{"seg-0.ts"});  // no temporary file left
    std::ifstream last(session / "seg-0.ts", std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(last)), std::istreambuf_iterator<char>());
    ASSERT_EQ(data.size(), kSegmentSize);
    EXPECT_EQ(static_cast<unsigned char>(data[0]), 99);

    // 4: removing a missing file is not an error; an existing file is removed.
    EXPECT_TRUE(store.remove_file(1, "seg-41.ts"));
    ASSERT_TRUE(store.write_file(1, "index.m3u8", reinterpret_cast<const std::uint8_t*>("#EXTM3U\n"), 8));
    EXPECT_TRUE(store.remove_file(1, "index.m3u8"));
    EXPECT_FALSE(fs::exists(session / "index.m3u8"));

    // 5: the directory is removed.
    store.close_session(1);
    EXPECT_FALSE(fs::exists(session));
    EXPECT_TRUE(fs::is_directory(hls));
    EXPECT_EQ(log.count(LogLevel::Error, "PAL-FS", ""), 0);
}

// open_session replaces a directory left from before by an empty one; a write of an empty file works.
TEST(Tst224, OpenSessionCreatesAnEmptyDirectoryAndEmptyFilesAreWritten) {
    TempDir tmp;
    RecordingLogger log;
    HlsStore store((tmp.path() / "hls").string(), &log);
    ASSERT_TRUE(store.reset());
    write_text(tmp.path() / "hls" / "2" / "seg-1.ts", "left over");
    ASSERT_TRUE(store.open_session(2));
    EXPECT_TRUE(names_in(tmp.path() / "hls" / "2").empty());
    ASSERT_TRUE(store.write_file(2, "empty.ts", nullptr, 0));
    EXPECT_EQ(fs::file_size(tmp.path() / "hls" / "2" / "empty.ts"), 0u);
    EXPECT_EQ(store.session_dir(2), (tmp.path() / "hls" / "2").string());
}

// Failures are reported by the return value and logged (ER-26 for the AL): invalid names, a closed
// session, an hls_dir that cannot be created, and the root directory as hls_dir.
TEST(Tst224, InvalidNamesAndFileSystemErrorsReturnFalseAndAreLogged) {
    TempDir tmp;
    RecordingLogger log;
    HlsStore store((tmp.path() / "hls").string(), &log);
    ASSERT_TRUE(store.reset());
    ASSERT_TRUE(store.open_session(1));
    const std::uint8_t byte = 0x47;
    for (const std::string& name : std::vector<std::string>{"", ".", "..", "a/b", ".tmp-1-seg-0.ts", "../escape.ts", std::string("a\0b", 3)}) {
        EXPECT_FALSE(store.write_file(1, name, &byte, 1)) << name;
        EXPECT_FALSE(store.remove_file(1, name)) << name;
    }
    EXPECT_FALSE(fs::exists(tmp.path() / "escape.ts"));
    EXPECT_FALSE(store.write_file(1, "seg-0.ts", nullptr, 5));

    store.close_session(1);
    EXPECT_FALSE(store.write_file(1, "seg-0.ts", &byte, 1));  // the session directory is gone
    EXPECT_FALSE(fs::exists(tmp.path() / "hls" / "1"));        // and is not created again
    EXPECT_GE(log.count(LogLevel::Error, "PAL-FS", "cannot write " + (tmp.path() / "hls" / "1" / "seg-0.ts").string()), 1);

    write_text(tmp.path() / "plain-file", "x");
    HlsStore under_file((tmp.path() / "plain-file" / "hls").string(), &log);
    EXPECT_FALSE(under_file.reset());
    EXPECT_GE(log.count(LogLevel::Error, "PAL-FS", "cannot create hls_dir"), 1);

    // The root directory is only checked with the guard itself: reset() is never run on it.
    for (const char* root : {"/", "//", "/..", "/./", "/tmp/.."}) EXPECT_FALSE(pal::hls_dir_is_safe(root)) << root;
    EXPECT_TRUE(pal::hls_dir_is_safe((tmp.path() / "hls").string()));
    EXPECT_TRUE(pal::hls_dir_is_safe("relative/hls"));
    HlsStore empty("", &log);
    EXPECT_FALSE(empty.reset());
    EXPECT_EQ(log.count(LogLevel::Error, "PAL-FS", "is empty or the root directory"), 1);
}

}  // namespace
