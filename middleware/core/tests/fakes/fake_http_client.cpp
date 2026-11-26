#include "fakes/fake_http_client.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#ifndef M15_FAKE_RESPONSES_DIR
#error "M15_FAKE_RESPONSES_DIR must be defined by the build"
#endif

namespace fakes {

namespace {

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

FakeHttpClient::Reply FakeHttpClient::Reply::ok(int status, std::string body, int delay_ms) {
    Reply r;
    r.response = pal::HttpResponse{true, status, std::move(body), ""};
    r.delay_ms = delay_ms;
    return r;
}

FakeHttpClient::Reply FakeHttpClient::Reply::file(int status, const std::string& name, int delay_ms) {
    return ok(status, read_response_file(name), delay_ms);
}

FakeHttpClient::Reply FakeHttpClient::Reply::transport_failure(int delay_ms) {
    Reply r;
    r.response = pal::HttpResponse{false, 0, "", "connection refused"};
    r.delay_ms = delay_ms;
    return r;
}

FakeHttpClient::Reply FakeHttpClient::Reply::held() const {
    Reply r = *this;
    r.hold = true;
    return r;
}

FakeHttpClient::FakeHttpClient(FakeClock& clock, CallLog* log, std::string mw_base_url,
                               std::string stream_control_url)
    : clock_(clock), log_(log), mw_base_url_(std::move(mw_base_url)),
      stream_control_url_(std::move(stream_control_url)) {}

FakeHttpClient::~FakeHttpClient() {
    for (const auto& entry : timers_) clock_.cancel_timer(entry.second);
}

pal::IHttpClient::RequestId FakeHttpClient::get(const std::string& url, int timeout_ms, Callback on_done) {
    return start_request("GET", url, "", timeout_ms, std::move(on_done));
}

pal::IHttpClient::RequestId FakeHttpClient::post_json(const std::string& url, const std::string& json_body,
                                                      int timeout_ms, Callback on_done) {
    return start_request("POST", url, json_body, timeout_ms, std::move(on_done));
}

void FakeHttpClient::cancel(RequestId id) {
    record(log_, "cancel " + std::to_string(id));
    Request* request = find(id);
    if (request == nullptr || request->state != State::Pending) return;
    request->state = State::Cancelled;
    auto timer = timers_.find(id);
    if (timer != timers_.end()) {
        clock_.cancel_timer(timer->second);
        timers_.erase(timer);
    }
    callbacks_.erase(id);   // never called after cancel() (Section 3.9.1, rule 4)
}

void FakeHttpClient::script(const std::string& method, const std::string& url, Reply reply) {
    scripts_[{method, url}].push_back(std::move(reply));
}

bool FakeHttpClient::release(RequestId id) {
    Request* request = find(id);
    if (request == nullptr || request->state != State::Pending) return false;
    const pal::HttpResponse response = request->reply.response;   // the callback may add requests
    deliver(id, response);
    return true;
}

bool FakeHttpClient::respond(RequestId id, const Reply& reply) {
    Request* request = find(id);
    if (request == nullptr || request->state != State::Pending) return false;
    auto timer = timers_.find(id);
    if (timer != timers_.end()) {
        clock_.cancel_timer(timer->second);
        timers_.erase(timer);
    }
    deliver(id, reply.response);
    return true;
}

std::size_t FakeHttpClient::count(const std::string& method, const std::string& url_prefix) const {
    std::size_t n = 0;
    for (const Request& r : requests_) {
        if (r.method == method && starts_with(r.url, url_prefix)) ++n;
    }
    return n;
}

const FakeHttpClient::Request* FakeHttpClient::last(const std::string& method,
                                                    const std::string& url_prefix) const {
    for (auto it = requests_.rbegin(); it != requests_.rend(); ++it) {
        if (it->method == method && starts_with(it->url, url_prefix)) return &*it;
    }
    return nullptr;
}

std::string FakeHttpClient::read_response_file(const std::string& name) {
    const std::string path = std::string(M15_FAKE_RESPONSES_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ADD_FAILURE() << "fake HTTP response file missing: " << path;
        return "";
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

pal::IHttpClient::RequestId FakeHttpClient::start_request(const std::string& method, const std::string& url,
                                                          const std::string& body, int timeout_ms,
                                                          Callback on_done) {
    const RequestId id = next_id_++;
    record(log_, method + " " + url);

    Request request;
    request.id = id;
    request.method = method;
    request.url = url;
    request.body = body;
    request.timeout_ms = timeout_ms;
    auto script = scripts_.find({method, url});
    if (script != scripts_.end() && !script->second.empty()) {
        request.reply = script->second.front();
        script->second.pop_front();
    } else {
        request.reply = default_reply(method, url);
    }
    const Reply reply = request.reply;
    requests_.push_back(std::move(request));
    callbacks_[id] = std::move(on_done);

    if (!reply.hold) {
        const bool times_out = reply.delay_ms > timeout_ms;
        const int delay = times_out ? timeout_ms : reply.delay_ms;
        const pal::HttpResponse response =
            times_out ? pal::HttpResponse{false, 0, "", "timeout"} : reply.response;
        timers_[id] = clock_.start_timer(delay, [this, id, response] {
            timers_.erase(id);
            deliver(id, response);
        });
    }
    return id;
}

FakeHttpClient::Reply FakeHttpClient::default_reply(const std::string& method, const std::string& url) const {
    if (method == "GET") {
        if (url == mw_base_url_ + "/services") return Reply::file(200, "mw_services.json");
        const std::string service_prefix = mw_base_url_ + "/services/";
        if (starts_with(url, service_prefix)) {
            const std::string id = url.substr(service_prefix.size());
            if (id == "svc-1" || id == "svc-2" || id == "svc-3" || id == "svc-4") {
                return Reply::file(200, "mw_service_" + id + ".json");
            }
            return Reply::file(404, "error_service_not_found.json");
        }
        if (starts_with(url, mw_base_url_ + "/epg?")) return Reply::file(200, "mw_epg.json");
    } else if (method == "POST") {
        if (url == stream_control_url_ + "/start") return Reply::file(200, "he_start.json");
        if (url == stream_control_url_ + "/keepalive") return Reply::file(200, "he_keepalive.json");
        if (url == stream_control_url_ + "/stop") return Reply::file(200, "he_stop.json");
    }
    ADD_FAILURE() << "fake HTTP client: no script and no default route for " << method << " " << url;
    return Reply::transport_failure();
}

void FakeHttpClient::deliver(RequestId id, const pal::HttpResponse& response) {
    Request* request = find(id);
    if (request == nullptr || request->state != State::Pending) return;
    request->state = State::Delivered;
    auto it = callbacks_.find(id);
    if (it == callbacks_.end()) return;
    Callback callback = std::move(it->second);
    callbacks_.erase(it);
    if (callback) callback(response);   // exactly once (Section 3.9.2)
}

FakeHttpClient::Request* FakeHttpClient::find(RequestId id) {
    for (Request& r : requests_) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

}  // namespace fakes
