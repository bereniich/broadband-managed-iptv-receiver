// PAL-FS: IHlsStore of IF-INT-04 (Section 3.9.2) on the Linux file system; B7 of Section 4.5.4.
#pragma once

#include <atomic>
#include <string>

#include "pal/platform.hpp"

namespace pal {

class HlsStore : public IHlsStore {
public:
    HlsStore(std::string hls_dir, ILogger* logger);

    // Platform::start(): creates hls_dir if needed and removes everything in it (files of an
    // earlier run). Returns false if it cannot be created or emptied.
    bool reset();

    bool open_session(SessionId session) override;
    // Atomic: the data goes to ".tmp-<n>-<name>" in the session directory, which is then renamed
    // to <name>. The temporary name starts with a dot, so it never matches "seg-*.ts".
    bool write_file(SessionId session, const std::string& name, const std::uint8_t* data,
                    std::size_t len) override;
    bool remove_file(SessionId session, const std::string& name) override;
    void close_session(SessionId session) override;

    std::string session_dir(SessionId session) const;

private:
    void log(LogLevel level, const std::string& message) const;

    std::string hls_dir_;
    ILogger* logger_;
    std::atomic<std::uint64_t> temp_counter_{0};
};

// False for an empty path and for any spelling of the root directory ("/", "//", "/..", …): such an
// hls_dir is never emptied.
bool hls_dir_is_safe(const std::string& hls_dir);

// A plain file name: not empty, no '/', no NUL, not "." or "..", not starting with '.'
// (names starting with '.' are reserved for the temporary files of write_file).
bool is_plain_file_name(const std::string& name);

}  // namespace pal
