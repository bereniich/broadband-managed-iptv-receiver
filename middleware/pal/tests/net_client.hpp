// Test clients of the PAL unit tests, written on plain sockets (not with Mongoose, which is the
// library under the code under test): an HTTP/1.1 client and a WebSocket client per RFC 6455 that
// checks Sec-WebSocket-Accept with its own SHA-1 (OpenSSL) and Base64.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace netclient {

inline int connect_local(std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

inline bool send_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// Receives into `buffer` until `enough(buffer)` or the timeout; false on timeout or close.
template <typename Pred>
bool recv_until(int fd, std::string& buffer, Pred enough, int timeout_ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!enough(buffer)) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(left.count())) <= 0) return false;
        char chunk[65536];
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(n));
    }
    return true;
}

struct HttpReply {
    bool answered = false;
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
};

// Reads one response with Content-Length from `buffer` (filled from fd as needed); `head` = response
// to HEAD (no body).
inline HttpReply read_reply(int fd, std::string& buffer, bool head = false) {
    HttpReply r;
    if (!recv_until(fd, buffer, [](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; })) return r;
    const std::size_t header_end = buffer.find("\r\n\r\n");
    const std::string head_text = buffer.substr(0, header_end);
    std::size_t line_end = head_text.find("\r\n");
    const std::string status_line = head_text.substr(0, line_end);
    if (status_line.size() < 12) return r;
    r.status = std::stoi(status_line.substr(9, 3));
    while (line_end != std::string::npos && line_end < head_text.size()) {
        const std::size_t next = head_text.find("\r\n", line_end + 2);
        const std::string line = head_text.substr(line_end + 2, (next == std::string::npos ? head_text.size() : next) - line_end - 2);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value[0] == ' ') value.erase(0, 1);
            r.headers[name] = value;
        }
        line_end = next;
    }
    std::size_t length = 0;
    if (r.headers.count("content-length") != 0) length = std::stoul(r.headers["content-length"]);
    if (head) length = 0;
    const std::size_t total = header_end + 4 + length;
    if (!recv_until(fd, buffer, [total](const std::string& b) { return b.size() >= total; })) return r;
    r.body = buffer.substr(header_end + 4, length);
    buffer.erase(0, total);
    r.answered = true;
    return r;
}

inline HttpReply http_request(std::uint16_t port, const std::string& method, const std::string& path) {
    const int fd = connect_local(port);
    if (fd < 0) return {};
    std::string buffer;
    send_all(fd, method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    HttpReply r = read_reply(fd, buffer, method == "HEAD");
    close(fd);
    return r;
}

inline std::string base64(const unsigned char* data, std::size_t len) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < len ? kAlphabet[(v >> 6) & 63] : '=';
        out += i + 2 < len ? kAlphabet[v & 63] : '=';
    }
    return out;
}

// Sec-WebSocket-Accept of RFC 6455, section 4.2.2.
inline std::string websocket_accept(const std::string& key) {
    const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(input.data(), input.size(), digest, &len, EVP_sha1(), nullptr);
    return base64(digest, len);
}

class WsClient {
public:
    // Connects to ws://127.0.0.1:<port><path>; connected() tells whether the handshake succeeded.
    WsClient(std::uint16_t port, const std::string& path = "/ws/control") {
        fd_ = connect_local(port);
        if (fd_ < 0) return;
        const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";  // the example key of RFC 6455
        send_all(fd_, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
                          "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n");
        const HttpReply r = read_reply(fd_, buffer_, true);
        handshake_status_ = r.status;
        accept_ok_ = r.headers.count("sec-websocket-accept") != 0 &&
                     r.headers.at("sec-websocket-accept") == websocket_accept(key);
        connected_ = r.answered && r.status == 101 && accept_ok_;
    }
    ~WsClient() { abort(); }
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    bool connected() const { return connected_; }
    int handshake_status() const { return handshake_status_; }

    // One masked frame (client to server); opcode 1 = text, 2 = binary.
    bool send_frame(const std::string& payload, int opcode = 1, bool fin = true) {
        std::string frame;
        frame += static_cast<char>((fin ? 0x80 : 0x00) | opcode);
        const std::size_t n = payload.size();
        if (n < 126) {
            frame += static_cast<char>(0x80 | n);
        } else if (n < 65536) {
            frame += static_cast<char>(0x80 | 126);
            frame += static_cast<char>(n >> 8);
            frame += static_cast<char>(n & 0xFF);
        } else {
            frame += static_cast<char>(0x80 | 127);
            for (int i = 7; i >= 0; --i) frame += static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFF);
        }
        const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
        frame.append(reinterpret_cast<const char*>(mask), 4);
        for (std::size_t i = 0; i < n; ++i) frame += static_cast<char>(payload[i] ^ mask[i % 4]);
        return send_all(fd_, frame);
    }
    bool send_text(const std::string& text) { return send_frame(text, 1); }

    // Next text frame from the server (server frames are not masked), or nullopt on timeout / close.
    std::optional<std::string> receive_text(int timeout_ms = 2000) {
        for (;;) {
            if (!recv_until(fd_, buffer_, [](const std::string& b) { return b.size() >= 2; }, timeout_ms)) return std::nullopt;
            const int opcode = static_cast<unsigned char>(buffer_[0]) & 0x0F;
            std::size_t len = static_cast<unsigned char>(buffer_[1]) & 0x7F;
            std::size_t header = 2;
            if (len == 126) {
                if (!recv_until(fd_, buffer_, [](const std::string& b) { return b.size() >= 4; }, timeout_ms)) return std::nullopt;
                len = (static_cast<unsigned char>(buffer_[2]) << 8) | static_cast<unsigned char>(buffer_[3]);
                header = 4;
            } else if (len == 127) {
                if (!recv_until(fd_, buffer_, [](const std::string& b) { return b.size() >= 10; }, timeout_ms)) return std::nullopt;
                len = 0;
                for (int i = 2; i < 10; ++i) len = (len << 8) | static_cast<unsigned char>(buffer_[i]);
                header = 10;
            }
            const std::size_t total = header + len;
            if (!recv_until(fd_, buffer_, [total](const std::string& b) { return b.size() >= total; }, timeout_ms)) return std::nullopt;
            std::string payload = buffer_.substr(header, len);
            buffer_.erase(0, total);
            if (opcode == 1) return payload;
            if (opcode == 8) return std::nullopt;  // close
        }
    }

    // True once the server has closed the connection (within the timeout).
    bool wait_closed(int timeout_ms = 2000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
            if (left.count() <= 0) return false;
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, static_cast<int>(left.count())) <= 0) return false;
            char chunk[4096];
            if (recv(fd_, chunk, sizeof(chunk), 0) <= 0) return true;  // end of stream or reset
        }
    }

    // Closes the socket without a close frame (abrupt disconnect).
    void abort() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
    std::string buffer_;
    bool connected_ = false;
    bool accept_ok_ = false;
    int handshake_status_ = 0;
};

inline std::uint16_t free_tcp_port() {
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

}  // namespace netclient
