// Content files of Section 5.2.1 for the tests marked "content" (Section 5.3.1). A missing file makes
// the test fail with "content missing: run content/generate.sh"; it is never skipped.
#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

#ifndef M15_CONTENT_DIR
#error "M15_CONTENT_DIR must be defined by the build (path of the content/ directory)"
#endif

namespace testutil {

constexpr const char* kContentMissing = "content missing: run content/generate.sh";

// Path of a file below content/, e.g. content_path("svc-1.ts") or content_path("test/novideo.ts").
inline std::string content_path(const std::string& name) {
    return std::string(M15_CONTENT_DIR) + "/" + name;
}

// True if the file can be read; otherwise records a non-fatal test failure with kContentMissing.
inline bool content_exists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (file.good()) return true;
    ADD_FAILURE() << kContentMissing << "\n  file: " << path;
    return false;
}

// Number of TS packets in `seconds` of content at the constant multiplex rate of 3 500 000 bit/s
// (Sections 4.7.2 and 5.2.1).
constexpr std::size_t packets_for_seconds(double seconds) {
    return static_cast<std::size_t>(seconds * 3500000.0 / 8.0 / 188.0);
}

}  // namespace testutil
