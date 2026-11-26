#include "pal_hls_store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace pal {

namespace fs = std::filesystem;

namespace {

constexpr const char* kModule = "PAL-FS";

}  // namespace

bool hls_dir_is_safe(const std::string& hls_dir) {
    if (hls_dir.empty()) return false;
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(fs::path(hls_dir), ec);
    if (ec) return false;
    return canonical != canonical.root_path();
}

bool is_plain_file_name(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name[0] == '.') return false;
    return name.find('/') == std::string::npos && name.find('\0') == std::string::npos;
}

HlsStore::HlsStore(std::string hls_dir, ILogger* logger) : hls_dir_(std::move(hls_dir)), logger_(logger) {}

void HlsStore::log(LogLevel level, const std::string& message) const {
    if (logger_ != nullptr) logger_->log(level, kModule, message);
}

std::string HlsStore::session_dir(SessionId session) const {
    return (fs::path(hls_dir_) / std::to_string(session)).string();
}

bool HlsStore::reset() {
    std::error_code ec;
    const fs::path dir(hls_dir_);
    if (!hls_dir_is_safe(hls_dir_)) {  // never empty the root directory because of a wrong configuration
        log(LogLevel::Error, "hls_dir '" + hls_dir_ + "' is empty or the root directory");
        return false;
    }
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
        log(LogLevel::Error, "cannot create hls_dir " + hls_dir_ + (ec ? ": " + ec.message() : ""));
        return false;
    }
    std::vector<fs::path> entries;  // collected first: no removal while the directory is read
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
    if (ec) {
        log(LogLevel::Error, "cannot read hls_dir " + hls_dir_ + ": " + ec.message());
        return false;
    }
    bool emptied = true;
    for (const fs::path& entry : entries) {
        std::error_code remove_error;
        fs::remove_all(entry, remove_error);
        if (remove_error) {
            log(LogLevel::Error, "cannot remove " + entry.string() + ": " + remove_error.message());
            emptied = false;
        }
    }
    return emptied;
}

bool HlsStore::open_session(SessionId session) {
    const fs::path dir = session_dir(session);
    std::error_code ec;
    fs::remove_all(dir, ec);  // a directory left from before is replaced by an empty one
    if (!ec) fs::create_directories(dir, ec);
    if (ec) {
        log(LogLevel::Error, "cannot create " + dir.string() + ": " + ec.message());
        return false;
    }
    return true;
}

bool HlsStore::write_file(SessionId session, const std::string& name, const std::uint8_t* data, std::size_t len) {
    if (!is_plain_file_name(name) || (data == nullptr && len > 0)) {
        log(LogLevel::Error, "write_file: invalid file name or data for session " + std::to_string(session));
        return false;
    }
    const fs::path dir = session_dir(session);
    const fs::path target = dir / name;
    const fs::path temp = dir / (".tmp-" + std::to_string(++temp_counter_) + "-" + name);
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        const int error = errno;
        log(LogLevel::Error, "cannot write " + target.string() + ": " + std::strerror(error));
        return false;
    }
    std::size_t done = 0;
    int error = 0;
    while (done < len) {
        const ssize_t n = ::write(fd, data + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = errno;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0 && error == 0) error = errno;
    if (error == 0 && ::rename(temp.c_str(), target.c_str()) != 0) error = errno;
    if (error != 0) {
        ::unlink(temp.c_str());
        log(LogLevel::Error, "cannot write " + target.string() + ": " + std::strerror(error));
        return false;
    }
    return true;
}

bool HlsStore::remove_file(SessionId session, const std::string& name) {
    if (!is_plain_file_name(name)) {
        log(LogLevel::Error, "remove_file: invalid file name for session " + std::to_string(session));
        return false;
    }
    const fs::path target = fs::path(session_dir(session)) / name;
    if (::unlink(target.c_str()) == 0) return true;
    const int error = errno;
    if (error == ENOENT) return true;  // a missing file is not an error
    log(LogLevel::Error, "cannot remove " + target.string() + ": " + std::strerror(error));
    return false;
}

void HlsStore::close_session(SessionId session) {
    const fs::path dir = session_dir(session);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) log(LogLevel::Error, "cannot remove " + dir.string() + ": " + ec.message());
}

}  // namespace pal
