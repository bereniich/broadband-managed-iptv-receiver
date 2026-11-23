// HLS store of the AL unit tests on a temporary directory (Sections 2.2.7 and 5.3.1, "AL fakes"),
// so that playlists and segments can be checked with ffprobe. Follows the contract of IHlsStore
// (Section 3.9.2); failures can be injected for the error paths (TST-127).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "fakes/call_log.hpp"
#include "pal/platform.hpp"

namespace fakes {

class FakeHlsStore : public pal::IHlsStore {
public:
    explicit FakeHlsStore(CallLog* log = nullptr);
    ~FakeHlsStore() override;

    bool open_session(SessionId session) override;
    bool write_file(SessionId session, const std::string& name, const std::uint8_t* data,
                    std::size_t len) override;
    bool remove_file(SessionId session, const std::string& name) override;
    void close_session(SessionId session) override;

    // ---- test side
    void fail_open(bool fail) { fail_open_ = fail; }
    // write_file returns false from the k-th call on (counted over all sessions, k >= 1).
    void fail_writes_from(std::size_t k) { fail_writes_from_ = k; }

    std::filesystem::path root() const { return root_; }
    std::filesystem::path dir(SessionId session) const { return root_ / std::to_string(session); }
    bool session_exists(SessionId session) const;
    std::vector<std::string> files(SessionId session) const;   // sorted names
    std::vector<std::uint8_t> read(SessionId session, const std::string& name) const;
    std::size_t write_calls() const { return write_calls_; }
    // Number of seg-*.ts files now / the largest number seen after any operation (B7).
    std::size_t segment_files(SessionId session) const;
    std::size_t max_segment_files(SessionId session) const;

private:
    void update_max(SessionId session);

    CallLog* log_;
    std::filesystem::path root_;
    bool fail_open_ = false;
    std::size_t fail_writes_from_ = 0;   // 0 = never
    std::size_t write_calls_ = 0;
    std::map<SessionId, std::size_t> max_segments_;
};

}  // namespace fakes
