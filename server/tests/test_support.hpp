// Helpers of the operator-side unit tests (Section 5.3.1): temporary directories, a minimal HTTP
// client over POSIX sockets and process control. Test code only; not part of the code under test.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace test {

// Temporary directory, removed with its content at the end of the test.
class TempDir {
public:
    TempDir() {
        std::string templ = (std::filesystem::temp_directory_path() / "m15-test-XXXXXX").string();
        if (mkdtemp(templ.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = templ;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const std::string& name) const { return (path_ / name).string(); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

inline std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

struct HttpReply {
    bool answered = false;  // a complete HTTP response was received
    int status = 0;
    std::string headers;  // header lines, lower case
    std::string body;
};

// Sends `request` as raw bytes to 127.0.0.1:<port> and reads one response (Content-Length) or
// until the server closes the connection; 5 s receive timeout.
inline HttpReply http_exchange(int port, const std::string& request) {
    HttpReply reply;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return reply;
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return reply;
    }
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    std::string data;
    size_t header_end = std::string::npos;
    long content_length = -1;
    char buf[4096];
    for (;;) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
        if (header_end == std::string::npos) {
            header_end = data.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string headers = data.substr(0, header_end);
                for (auto& c : headers) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const size_t cl = headers.find("content-length:");
                if (cl != std::string::npos) content_length = std::strtol(headers.c_str() + cl + 15, nullptr, 10);
            }
        }
        if (header_end != std::string::npos && content_length >= 0 &&
            data.size() >= header_end + 4 + static_cast<size_t>(content_length)) {
            break;
        }
    }
    close(fd);
    if (header_end == std::string::npos || data.compare(0, 5, "HTTP/") != 0) return reply;
    reply.answered = true;
    reply.status = std::atoi(data.c_str() + data.find(' ') + 1);
    reply.headers = data.substr(0, header_end);
    for (auto& c : reply.headers) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    reply.body = data.substr(header_end + 4);
    return reply;
}

inline HttpReply http_get(int port, const std::string& target) {
    return http_exchange(port, "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
}

// A free TCP port on 127.0.0.1 (bound and released; good enough for a test).
inline int free_tcp_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    close(fd);
    return ntohs(addr.sin_port);
}

// A child process with standard output and standard error redirected to `output_file`.
class Process {
public:
    Process(const std::vector<std::string>& argv, const std::string& output_file) {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 1, output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        if (posix_spawn(&pid_, args[0], &actions, nullptr, args.data(), environ) != 0) pid_ = -1;
        posix_spawn_file_actions_destroy(&actions);
    }
    ~Process() {
        if (pid_ > 0 && !exited_) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
    }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool started() const { return pid_ > 0; }
    void signal(int sig) const { kill(pid_, sig); }

    // Exit code, or -1 if the process did not exit normally within `timeout`.
    int wait(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                exited_ = true;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return -1;
    }

private:
    pid_t pid_ = -1;
    bool exited_ = false;
};

}  // namespace test
