#include "pal_http_client.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>

#include "mongoose.h"

namespace pal {

namespace {

constexpr const char* kModule = "PAL-HTTP";

HttpResponse failure(const std::string& error) {
    HttpResponse r;
    r.transport_ok = false;
    r.status = 0;
    r.error = error;
    return r;
}

// "http://<IPv4>[:port]/…" only: no TLS, and no host name (Mongoose would resolve it through an
// external DNS server).
bool url_is_supported(const std::string& url, std::string* why) {
    if (url.compare(0, 7, "http://") != 0) {
        *why = "only http:// URLs are supported";
        return false;
    }
    const mg_str host = mg_url_host(url.c_str());
    const std::string host_text(host.buf != nullptr ? host.buf : "", host.len);
    in_addr address{};
    if (host_text.empty() || inet_pton(AF_INET, host_text.c_str(), &address) != 1) {
        *why = "the host must be an IPv4 address";
        return false;
    }
    if (mg_url_port(url.c_str()) == 0) {
        *why = "invalid port";
        return false;
    }
    return true;
}

// The request id travels in the connection's scratch area c->data.
void set_request_id(mg_connection* c, IHttpClient::RequestId id) { std::memcpy(c->data, &id, sizeof(id)); }
IHttpClient::RequestId request_id(const mg_connection* c) {
    IHttpClient::RequestId id;
    std::memcpy(&id, c->data, sizeof(id));
    return id;
}

}  // namespace

HttpClient::HttpClient(EventLoop& loop, ILogger* logger) : loop_(loop), logger_(logger) {}

void HttpClient::log(LogLevel level, const std::string& message) const {
    if (logger_ != nullptr) logger_->log(level, kModule, message);
}

IHttpClient::RequestId HttpClient::get(const std::string& url, int timeout_ms, Callback on_done) {
    return start("GET", url, std::string(), timeout_ms, std::move(on_done));
}

IHttpClient::RequestId HttpClient::post_json(const std::string& url, const std::string& json_body, int timeout_ms,
                                             Callback on_done) {
    return start("POST", url, json_body, timeout_ms, std::move(on_done));
}

IHttpClient::RequestId HttpClient::start(const char* method, const std::string& url, const std::string& body,
                                         int timeout_ms, Callback on_done) {
    const RequestId id = next_id_++;
    try {
        if (!on_done) return id;  // nothing to call; the request is not sent
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_[id].on_done = std::move(on_done);
        }
        // The timeout runs from now, also while the request waits in the command queue.
        const auto timer = loop_.add_timer(std::max(timeout_ms, 1), [this, id, timeout_ms] {
            finish(id, failure("timeout after " + std::to_string(timeout_ms) + " ms"));
        });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_.find(id);
            if (it != pending_.end()) it->second.timer = timer;
        }

        std::string why;
        if (!url_is_supported(url, &why)) {
            loop_.post([this, id, why](mg_mgr&) { finish(id, failure(why)); });  // never inside get()
            return id;
        }
        const mg_str host = mg_url_host(url.c_str());
        std::string request = std::string(method) + " " + mg_url_uri(url.c_str()) + " HTTP/1.1\r\nHost: " +
                              std::string(host.buf, host.len) + ":" + std::to_string(mg_url_port(url.c_str())) +
                              "\r\nConnection: close\r\n";
        if (std::strcmp(method, "POST") == 0) {
            request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        }
        request += "\r\n";
        request += body;
        loop_.post([this, id, url, request = std::move(request)](mg_mgr& mgr) {
            if (!is_pending(id)) return;  // cancelled or timed out while queued
            mg_connection* c = mg_http_connect(&mgr, url.c_str(), &HttpClient::on_event, this);
            if (c == nullptr) {
                finish(id, failure("cannot create the connection"));
                return;
            }
            set_request_id(c, id);
            unsent_[c->id] = request;
        });
    } catch (...) {
        // Section 3.9.1, rule 5: no exception leaves the interface.
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);
    }
    return id;
}

bool HttpClient::is_pending(RequestId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.count(id) != 0;
}

void HttpClient::cancel(RequestId id) {
    EventLoop::TimerId timer = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto it = pending_.find(id);
        if (it != pending_.end()) {
            timer = it->second.timer;
            pending_.erase(it);  // the connection notices it at its next poll and closes
        }
        if (!loop_.in_loop_thread()) done_.wait(lock, [this, id] { return firing_ != id; });
    }
    if (timer != 0) loop_.cancel_timer(timer);
}

void HttpClient::finish(RequestId id, HttpResponse response) {
    Callback callback;
    EventLoop::TimerId timer = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(id);
        if (it == pending_.end()) return;  // already delivered or cancelled
        callback = std::move(it->second.on_done);
        timer = it->second.timer;
        pending_.erase(it);
        if (!callbacks_enabled_) return;
        firing_ = id;
    }
    if (timer != 0) loop_.cancel_timer(timer);  // on the loop thread: does not wait
    try {
        callback(response);
    } catch (...) {
        log(LogLevel::Error, "an HTTP callback threw an exception");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        firing_ = 0;
    }
    done_.notify_all();
}

void HttpClient::on_event(mg_connection* c, int ev, void* ev_data) {
    auto* self = static_cast<HttpClient*>(c->fn_data);
    const RequestId id = request_id(c);
    switch (ev) {
        case MG_EV_CONNECT: {
            const auto it = self->unsent_.find(c->id);
            if (it != self->unsent_.end()) {
                mg_send(c, it->second.data(), it->second.size());
                self->unsent_.erase(it);
            }
            break;
        }
        case MG_EV_HTTP_MSG: {
            const auto* hm = static_cast<mg_http_message*>(ev_data);
            HttpResponse r;
            r.transport_ok = true;
            r.status = mg_http_status(hm);
            r.body.assign(hm->body.buf, hm->body.len);
            self->finish(id, std::move(r));
            c->is_closing = 1;
            break;
        }
        case MG_EV_ERROR:
            self->finish(id, failure(ev_data != nullptr ? static_cast<const char*>(ev_data) : "connection error"));
            break;
        case MG_EV_POLL:
            if (!self->is_pending(id)) c->is_closing = 1;  // cancelled or timed out
            break;
        case MG_EV_CLOSE:
            self->unsent_.erase(c->id);
            self->finish(id, failure("connection closed before the response"));
            break;
        default:
            break;
    }
}

}  // namespace pal
