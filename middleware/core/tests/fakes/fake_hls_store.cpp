#include "fakes/fake_hls_store.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <system_error>

namespace fakes {

namespace fs = std::filesystem;

namespace {

// A plain file name such as "seg-17.ts" or "index.m3u8" (Section 3.9.2).
bool plain_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos;
}

bool is_segment(const std::string& name) {
    return name.size() > 7 && name.compare(0, 4, "seg-") == 0 &&
           name.compare(name.size() - 3, 3, ".ts") == 0;
}

fs::path unique_root() {
    std::random_device device;
    std::ostringstream name;
    name << "m15-al-tests-" << std::hex << device() << device();
    return fs::temp_directory_path() / name.str();
}

}  // namespace

FakeHlsStore::FakeHlsStore(CallLog* log) : log_(log), root_(unique_root()) {
    fs::create_directories(root_);
}

FakeHlsStore::~FakeHlsStore() {
    std::error_code ec;
    fs::remove_all(root_, ec);
}

bool FakeHlsStore::open_session(SessionId session) {
    record(log_, "open_session " + std::to_string(session));
    if (fail_open_) return false;
    std::error_code ec;
    fs::remove_all(dir(session), ec);
    fs::create_directories(dir(session), ec);
    update_max(session);
    return !ec;
}

bool FakeHlsStore::write_file(SessionId session, const std::string& name, const std::uint8_t* data,
                              std::size_t len) {
    record(log_, "write_file " + std::to_string(session) + " " + name + " " + std::to_string(len));
    ++write_calls_;
    if (fail_writes_from_ != 0 && write_calls_ >= fail_writes_from_) return false;
    if (!plain_name(name) || !session_exists(session)) return false;

    // Atomic: write under a temporary name, then rename (Section 3.9.2).
    const fs::path target = dir(session) / name;
    const fs::path temporary = dir(session) / ("." + name + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(temporary, target, ec);
    update_max(session);
    if (ec) return false;
    if (write_hook_) write_hook_(session, name);
    return true;
}

bool FakeHlsStore::remove_file(SessionId session, const std::string& name) {
    record(log_, "remove_file " + std::to_string(session) + " " + name);
    if (!plain_name(name)) return false;
    std::error_code ec;
    fs::remove(dir(session) / name, ec);   // a missing file is not an error
    update_max(session);
    return !ec;
}

void FakeHlsStore::close_session(SessionId session) {
    record(log_, "close_session " + std::to_string(session));
    std::error_code ec;
    fs::remove_all(dir(session), ec);
}

bool FakeHlsStore::session_exists(SessionId session) const {
    std::error_code ec;
    return fs::is_directory(dir(session), ec);
}

std::vector<std::string> FakeHlsStore::files(SessionId session) const {
    std::vector<std::string> names;
    std::error_code ec;
    if (!session_exists(session)) return names;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir(session), ec)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::uint8_t> FakeHlsStore::read(SessionId session, const std::string& name) const {
    std::ifstream in(dir(session) / name, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::size_t FakeHlsStore::segment_files(SessionId session) const {
    const std::vector<std::string> names = files(session);
    return static_cast<std::size_t>(std::count_if(names.begin(), names.end(), is_segment));
}

std::size_t FakeHlsStore::max_segment_files(SessionId session) const {
    auto it = max_segments_.find(session);
    return it == max_segments_.end() ? 0 : it->second;
}

void FakeHlsStore::update_max(SessionId session) {
    std::size_t& max = max_segments_[session];
    max = std::max(max, segment_files(session));
}

}  // namespace fakes
