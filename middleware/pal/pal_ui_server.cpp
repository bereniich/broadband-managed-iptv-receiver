#include "pal_ui_server.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <future>
#include <vector>

#include "mongoose.h"
#include "pal_hls_store.hpp"

namespace pal {

namespace {

constexpr const char* kModule = "PAL-WS";
constexpr const char* kControlPath = "/ws/control";
constexpr std::size_t kMaxFileSize = 64u << 20;  // larger than any Web or HLS file of the system

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string to_string(const mg_str& s) { return std::string(s.buf != nullptr ? s.buf : "", s.len); }

}  // namespace

std::string map_web_path(const std::string& root, const std::string& decoded_path) {
    if (decoded_path.empty() || decoded_path[0] != '/' || decoded_path.find('\0') != std::string::npos) return "";
    std::string relative;
    std::size_t pos = 1;
    while (pos <= decoded_path.size()) {
        const std::size_t next = std::min(decoded_path.find('/', pos), decoded_path.size());
        const std::string part = decoded_path.substr(pos, next - pos);
        if (!part.empty()) {
            if (part[0] == '.') return "";  // "..", "." and hidden files
            relative += "/" + part;
        }
        pos = next + 1;
    }
    std::string path = root + relative;
    struct stat st {};
    if (relative.empty() || (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))) path += "/index.html";
    return path;
}

std::string content_type_for(const std::string& path) {
    static const struct {
        const char* ext;
        const char* type;
    } kTypes[] = {
        {".m3u8", "application/vnd.apple.mpegurl"},  // IF-INT-02
        {".ts", "video/mp2t"},                        // IF-INT-02
        {".html", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},    // ES modules need a JavaScript type
        {".mjs", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".ico", "image/x-icon"},
        {".woff2", "font/woff2"},
        {".txt", "text/plain; charset=utf-8"},
    };
    for (const auto& t : kTypes) {
        if (ends_with(path, t.ext)) return t.type;
    }
    return "application/octet-stream";
}

UiServer::UiServer(EventLoop& loop, ILogger* logger, std::string webui_dir, std::string hls_dir)
    : loop_(loop), logger_(logger), webui_dir_(std::move(webui_dir)), hls_dir_(std::move(hls_dir)) {}

void UiServer::log(LogLevel level, const std::string& message) const {
    if (logger_ != nullptr) logger_->log(level, kModule, message);
}

void UiServer::set_handlers(MessageCallback on_message, ConnectionCallback on_connection) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    on_message_ = std::move(on_message);
    on_connection_ = std::move(on_connection);
}

bool UiServer::listen(std::uint16_t port) {
    auto result = std::make_shared<std::promise<std::uint16_t>>();
    std::future<std::uint16_t> answer = result->get_future();
    loop_.post([this, port, result](mg_mgr& mgr) {
        const std::string url = "http://127.0.0.1:" + std::to_string(port);
        mg_connection* listener = mg_http_listen(&mgr, url.c_str(), &UiServer::on_event, this);
        result->set_value(listener != nullptr ? mg_ntohs(listener->loc.port) : 0);
    });
    std::uint16_t bound = 0;
    try {
        if (answer.wait_for(std::chrono::seconds(5)) == std::future_status::ready) bound = answer.get();
    } catch (...) {
        bound = 0;  // the loop stopped before it ran the command
    }
    if (bound == 0) {
        log(LogLevel::Error, "cannot listen on 127.0.0.1:" + std::to_string(port) + " (port in use?)");
        return false;
    }
    port_ = bound;
    log(LogLevel::Info, "listening on http://127.0.0.1:" + std::to_string(port_.load()) + "/ webui_dir=" + webui_dir_ +
                            " hls_dir=" + hls_dir_);
    return true;
}

void UiServer::send(ClientId client, const std::string& text) {
    loop_.post([this, client, text](mg_mgr& mgr) {
        if (clients_.count(static_cast<unsigned long>(client)) == 0) return;
        for (mg_connection* c = mgr.conns; c != nullptr; c = c->next) {
            if (c->id == client && c->is_websocket) {
                mg_ws_send(c, text.data(), text.size(), WEBSOCKET_OP_TEXT);
                return;
            }
        }
    });
}

void UiServer::broadcast(const std::string& text) {
    loop_.post([this, text](mg_mgr& mgr) {
        for (mg_connection* c = mgr.conns; c != nullptr; c = c->next) {
            if (c->is_websocket && clients_.count(c->id) != 0) mg_ws_send(c, text.data(), text.size(), WEBSOCKET_OP_TEXT);
        }
    });
}

void UiServer::serve_file(mg_connection* c, bool head, const std::string& path, const std::string& content_type,
                          const char* extra_headers) {
    const int fd = path.empty() ? -1 : ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::size_t>(st.st_size) > kMaxFileSize) {
        if (fd >= 0) ::close(fd);
        mg_http_reply(c, 404, "Content-Type: text/plain\r\n", "Not found\n");
        return;
    }
    std::vector<char> data(static_cast<std::size_t>(st.st_size));
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::read(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (done != data.size()) {
        mg_http_reply(c, 500, "Content-Type: text/plain\r\n", "Read error\n");
        return;
    }
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\n%s\r\n", content_type.c_str(),
              static_cast<unsigned long>(data.size()), extra_headers);
    if (!head && !data.empty()) mg_send(c, data.data(), data.size());
    c->is_resp = 0;  // response complete: Mongoose may parse the next request of this connection
}

void UiServer::handle_http(mg_connection* c, mg_http_message* hm) {
    const std::string method = to_string(hm->method);
    const std::string uri = to_string(hm->uri);
    if (uri == kControlPath) {
        mg_ws_upgrade(c, hm, nullptr);  // 426 without the WebSocket headers
        return;
    }
    const bool head = method == "HEAD";
    if (method != "GET" && !head) {
        mg_http_reply(c, 404, "Content-Type: text/plain\r\n", "Not found\n");
        return;
    }
    std::string decoded(uri.size() + 1, '\0');
    const int n = mg_url_decode(uri.c_str(), uri.size(), &decoded[0], decoded.size(), 0);
    decoded.resize(n > 0 ? static_cast<std::size_t>(n) : 0);

    if (decoded.compare(0, 5, "/hls/") == 0) {
        // /hls/<session>/<file>: a decimal session number and a plain file name (no temporary file).
        const std::size_t slash = decoded.find('/', 5);
        const std::string session = slash == std::string::npos ? "" : decoded.substr(5, slash - 5);
        const std::string name = slash == std::string::npos ? "" : decoded.substr(slash + 1);
        bool ok = !session.empty() && session.size() <= 20 && is_plain_file_name(name) &&
                  (ends_with(name, ".m3u8") || ends_with(name, ".ts"));
        for (char ch : session) ok = ok && std::isdigit(static_cast<unsigned char>(ch));
        const std::string path = ok ? hls_dir_ + "/" + session + "/" + name : "";
        serve_file(c, head, path, content_type_for(name),
                   ends_with(name, ".m3u8") ? "Cache-Control: no-cache\r\n" : "");
        return;
    }
    const std::string path = map_web_path(webui_dir_, decoded);
    serve_file(c, head, path, content_type_for(path), "");
}

void UiServer::on_event(mg_connection* c, int ev, void* ev_data) {
    auto* self = static_cast<UiServer*>(c->fn_data);
    switch (ev) {
        case MG_EV_HTTP_MSG:
            self->handle_http(c, static_cast<mg_http_message*>(ev_data));
            break;
        case MG_EV_WS_OPEN: {
            self->clients_.insert(c->id);
            self->log(LogLevel::Info, "control client " + std::to_string(c->id) + " connected");
            if (!self->callbacks_enabled_) break;
            ConnectionCallback handler;
            {
                std::lock_guard<std::mutex> lock(self->handler_mutex_);
                handler = self->on_connection_;
            }
            try {
                if (handler) handler(c->id, true);
            } catch (...) {
                self->log(LogLevel::Error, "the connection callback threw an exception");
            }
            break;
        }
        case MG_EV_WS_MSG: {
            const auto* wm = static_cast<mg_ws_message*>(ev_data);
            if ((wm->flags & 0x0F) != WEBSOCKET_OP_TEXT) {
                self->log(LogLevel::Warn, "binary frame from control client " + std::to_string(c->id) + " ignored");
                break;
            }
            if (!self->callbacks_enabled_) break;
            MessageCallback handler;
            {
                std::lock_guard<std::mutex> lock(self->handler_mutex_);
                handler = self->on_message_;
            }
            try {
                if (handler) handler(c->id, to_string(wm->data));  // the content is never logged (keys, REQ-55)
            } catch (...) {
                self->log(LogLevel::Error, "the message callback threw an exception");
            }
            break;
        }
        case MG_EV_CLOSE: {
            if (self->clients_.erase(c->id) == 0) break;
            self->log(LogLevel::Info, "control client " + std::to_string(c->id) + " disconnected");
            if (!self->callbacks_enabled_) break;
            ConnectionCallback handler;
            {
                std::lock_guard<std::mutex> lock(self->handler_mutex_);
                handler = self->on_connection_;
            }
            try {
                if (handler) handler(c->id, false);
            } catch (...) {
                self->log(LogLevel::Error, "the connection callback threw an exception");
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace pal
