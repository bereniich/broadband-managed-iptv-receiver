// HTTP and JSON helpers of the operator side over Mongoose and nlohmann/json (Section 4.7).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <json.hpp>

#include "log.hpp"

struct mg_mgr;
struct mg_connection;

namespace srv {

struct HttpRequest {
    std::string method;
    std::string path;   // URL-decoded path, e.g. "/api/v1/epg"
    std::string query;  // raw query string without '?', e.g. "from=1&to=2"
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string body;  // JSON text; sent with Content-Type: application/json
    // Set by error_response(), for the log line of the request.
    std::string error_code;
    std::string error_message;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// JSON text of a value; invalid UTF-8 (e.g. echoed from a request) is replaced, never thrown.
std::string json_text(const nlohmann::ordered_json& value);

HttpResponse json_response(int status, const nlohmann::ordered_json& body);

// Error body of Sections 3.1 and 3.2: {"error": {"code": "<CODE>", "message": "<text>"}}.
HttpResponse error_response(int status, std::string_view code, std::string_view message);

// Value of a query variable, URL-decoded ('+' is a space); nullopt if the variable is missing or
// its encoding is invalid. The first occurrence wins.
std::optional<std::string> query_var(std::string_view query, std::string_view name);

// Percent-decoding; nullopt for an invalid '%' sequence.
std::optional<std::string> url_decode(std::string_view text, bool plus_is_space);

// Strict decimal integer: optional '-', 1 – 19 digits, within int64_t; nothing else.
std::optional<int64_t> parse_int64(std::string_view text);

// HTTP server on 127.0.0.1 only (HLR-20) with one event-loop thread (Section 4.5.2). The handler
// runs on that thread, one request at a time; its response is sent with
// Content-Type: application/json. A request Mongoose cannot parse (e.g. random bytes) is dropped
// by Mongoose by closing the connection; the server keeps running (REQ-58).
class HttpServer {
public:
    // `logger` (optional) receives an error line if the handler throws.
    explicit HttpServer(HttpHandler handler, Logger* logger = nullptr, std::string module = "SRV");
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Listens on 127.0.0.1:<port> (0 = an ephemeral port, for tests) and starts the event-loop
    // thread. Returns false, with `error` set, if the port cannot be bound.
    bool start(int port, std::string* error = nullptr);

    // Port actually bound by start().
    int port() const { return port_; }

    // Returns after the event-loop thread has ended; no handler call after that.
    void stop();

private:
    static void on_event(mg_connection* c, int ev, void* ev_data);

    HttpHandler handler_;
    Logger* logger_;
    std::string module_;
    std::unique_ptr<mg_mgr> mgr_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int port_ = 0;
};

// Routes the diagnostic lines of Mongoose (e.g. "HTTP parse" for a request that is not HTTP) to
// `logger` as WARN lines of `module`, instead of standard output. Process-wide; call once from
// main() before the server starts. Passing nullptr restores the Mongoose default.
void route_mongoose_log(Logger* logger, const std::string& module);

}  // namespace srv
