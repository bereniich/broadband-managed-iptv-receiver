#include "http.hpp"

#include <cerrno>
#include <cstdlib>
#include <mutex>

#include <mongoose.h>

namespace srv {

std::string json_text(const nlohmann::ordered_json& value) {
    return value.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

HttpResponse json_response(int status, const nlohmann::ordered_json& body) {
    HttpResponse r;
    r.status = status;
    r.body = json_text(body);
    return r;
}

HttpResponse error_response(int status, std::string_view code, std::string_view message) {
    nlohmann::ordered_json error;
    error["code"] = std::string(code);
    error["message"] = std::string(message);
    nlohmann::ordered_json body;
    body["error"] = std::move(error);
    HttpResponse r = json_response(status, body);
    r.error_code = std::string(code);
    r.error_message = std::string(message);
    return r;
}

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::optional<std::string> url_decode(std::string_view text, bool plus_is_space) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size()) return std::nullopt;
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            out += static_cast<char>(hi * 16 + lo);
            i += 2;
        } else if (c == '+' && plus_is_space) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::optional<std::string> query_var(std::string_view query, std::string_view name) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string_view::npos) end = query.size();
        const std::string_view pair = query.substr(pos, end - pos);
        const size_t eq = pair.find('=');
        const auto key = url_decode(pair.substr(0, eq), true);
        if (key && *key == name) {
            if (eq == std::string_view::npos) return std::string();
            return url_decode(pair.substr(eq + 1), true);
        }
        pos = end + 1;
    }
    return std::nullopt;
}

std::optional<int64_t> parse_int64(std::string_view text) {
    size_t i = 0;
    if (!text.empty() && text[0] == '-') i = 1;
    const size_t digits = text.size() - i;
    if (digits == 0 || digits > 19) return std::nullopt;
    for (size_t k = i; k < text.size(); ++k) {
        if (text[k] < '0' || text[k] > '9') return std::nullopt;
    }
    const std::string s(text);
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return std::nullopt;
    return static_cast<int64_t>(value);
}

HttpServer::HttpServer(HttpHandler handler, Logger* logger, std::string module)
    : handler_(std::move(handler)), logger_(logger), module_(std::move(module)) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(int port, std::string* error) {
    if (running_) {
        if (error) *error = "server already started";
        return false;
    }
    if (mg_log_level > MG_LL_ERROR) mg_log_set(MG_LL_ERROR);
    mgr_ = std::make_unique<mg_mgr>();
    mg_mgr_init(mgr_.get());
    const std::string url = "http://127.0.0.1:" + std::to_string(port);
    mg_connection* listener = mg_http_listen(mgr_.get(), url.c_str(), &HttpServer::on_event, this);
    if (listener == nullptr) {
        mg_mgr_free(mgr_.get());
        mgr_.reset();
        if (error) *error = "cannot listen on 127.0.0.1:" + std::to_string(port) + " (port in use?)";
        return false;
    }
    port_ = mg_ntohs(listener->loc.port);
    running_ = true;
    thread_ = std::thread([this] {
        while (running_) mg_mgr_poll(mgr_.get(), 20);
    });
    return true;
}

void HttpServer::stop() {
    if (!mgr_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    mg_mgr_free(mgr_.get());
    mgr_.reset();
}

void HttpServer::on_event(mg_connection* c, int ev, void* ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;
    auto* self = static_cast<HttpServer*>(c->fn_data);
    const auto* hm = static_cast<const mg_http_message*>(ev_data);

    HttpRequest request;
    request.method.assign(hm->method.buf, hm->method.len);
    const std::string_view raw_path(hm->uri.buf, hm->uri.len);
    const auto path = url_decode(raw_path, false);
    request.path = path ? *path : std::string(raw_path);
    request.query.assign(hm->query.buf, hm->query.len);
    request.body.assign(hm->body.buf, hm->body.len);

    HttpResponse response;
    try {
        response = self->handler_(request);
    } catch (const std::exception& e) {
        if (self->logger_) self->logger_->error(self->module_, std::string("request handler failed: ") + e.what());
        response = error_response(400, "INVALID_REQUEST", "request could not be processed");
    } catch (...) {
        if (self->logger_) self->logger_->error(self->module_, "request handler failed");
        response = error_response(400, "INVALID_REQUEST", "request could not be processed");
    }
    mg_http_reply(c, response.status, "Content-Type: application/json\r\n", "%s", response.body.c_str());
}

namespace {

std::mutex g_mg_log_mutex;
Logger* g_mg_logger = nullptr;
std::string g_mg_module;
std::string g_mg_line;

void mongoose_log_char(char ch, void*) {
    std::lock_guard<std::mutex> lock(g_mg_log_mutex);
    if (g_mg_logger == nullptr) return;
    if (ch == '\n') {
        while (!g_mg_line.empty() && (g_mg_line.back() == '\r' || g_mg_line.back() == ' ')) g_mg_line.pop_back();
        if (!g_mg_line.empty()) g_mg_logger->warn(g_mg_module, "http: " + g_mg_line);
        g_mg_line.clear();
    } else if (g_mg_line.size() < 1024) {
        g_mg_line += ch;
    }
}

}  // namespace

void route_mongoose_log(Logger* logger, const std::string& module) {
    std::lock_guard<std::mutex> lock(g_mg_log_mutex);
    g_mg_logger = logger;
    g_mg_module = module;
    g_mg_line.clear();
    if (logger != nullptr) {
        mg_log_set(MG_LL_ERROR);
        mg_log_set_fn(&mongoose_log_char, nullptr);
    } else {
        mg_log_set_fn(&mg_pfn_stdout, nullptr);
    }
}

}  // namespace srv
