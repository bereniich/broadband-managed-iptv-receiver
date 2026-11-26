// PAL-WS: the HTTP / WebSocket server of the middleware on 127.0.0.1:<ui_http_port> (Section 4.2.5),
// on the PAL event loop:
//   - IF-INT-01: WebSocket /ws/control, one text frame = one message (IUiChannel, Section 3.9.2);
//   - IF-INT-02: GET /hls/<session>/<file> from hls_dir (Section 3.7);
//   - IF-INT-03: GET /<path> from webui_dir, "/" = index.html (Section 3.8); unknown -> 404.
// Files are opened, measured with fstat and read through the same descriptor, so a playlist that
// AL-OUT replaces at that moment is sent either old or new but always whole, and a segment removed
// while it is sent stays readable (Section 4.5.4, B7).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

#include "pal/platform.hpp"
#include "pal_event_loop.hpp"

namespace pal {

class UiServer : public IUiChannel {
public:
    UiServer(EventLoop& loop, ILogger* logger, std::string webui_dir, std::string hls_dir);

    void set_handlers(MessageCallback on_message, ConnectionCallback on_connection) override;
    // Copied and posted to the event loop; frames to one client keep their order. A client that is
    // gone is skipped.
    void send(ClientId client, const std::string& text) override;
    void broadcast(const std::string& text) override;

    // Platform::start(): listens on 127.0.0.1:<port> (0 = ephemeral, for tests) through the running
    // event loop. Returns false, logged, if the port cannot be opened.
    bool listen(std::uint16_t port);
    std::uint16_t port() const { return port_; }
    // Platform::stop(): connections closed by the stop do not reach the handlers.
    void disable_callbacks() { callbacks_enabled_ = false; }

private:
    static void on_event(struct mg_connection* c, int ev, void* ev_data);
    void handle_http(struct mg_connection* c, struct mg_http_message* hm);
    void serve_file(struct mg_connection* c, bool head, const std::string& path, const std::string& content_type,
                    const char* extra_headers);
    void log(LogLevel level, const std::string& message) const;

    EventLoop& loop_;
    ILogger* logger_;
    std::string webui_dir_;
    std::string hls_dir_;
    std::atomic<std::uint16_t> port_{0};
    std::atomic<bool> callbacks_enabled_{true};

    std::mutex handler_mutex_;
    MessageCallback on_message_;
    ConnectionCallback on_connection_;

    std::set<unsigned long> clients_;  // open WebSocket connections; loop thread only
};

// Maps an already URL-decoded request path to a file below `root`: "" for a path that leaves the
// root, names a hidden entry (a segment starting with '.') or contains a NUL; "/" and directories
// map to their index.html.
std::string map_web_path(const std::string& root, const std::string& decoded_path);

// Content type by file extension (IF-INT-02 types for .m3u8 and .ts).
std::string content_type_for(const std::string& path);

}  // namespace pal
