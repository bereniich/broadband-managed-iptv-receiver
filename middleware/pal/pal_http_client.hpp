// PAL-HTTP: IHttpClient of IF-INT-04 (Section 3.9.2) over Mongoose, on the PAL event loop.
//
// get() and post_json() copy their arguments and post the request to the event loop (Section 4.5.2);
// the callback is called exactly once on the event-loop thread: with the response, or with
// transport_ok = false for a connection failure, an invalid URL or the timeout. cancel() returning
// guarantees that the callback is never called (Section 3.9.1, rule 4).
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>

#include "pal/platform.hpp"
#include "pal_event_loop.hpp"

namespace pal {

class HttpClient : public IHttpClient {
public:
    HttpClient(EventLoop& loop, ILogger* logger);

    // Only http:// URLs with an IPv4 address as host (e.g. http://127.0.0.1:8080/api/v1/services);
    // any other URL gets transport_ok = false. A timeout below 1 ms counts as 1 ms.
    RequestId get(const std::string& url, int timeout_ms, Callback on_done) override;
    RequestId post_json(const std::string& url, const std::string& json_body, int timeout_ms,
                        Callback on_done) override;
    void cancel(RequestId id) override;

    // Platform::stop(): from now on no callback is called (also not for connections closed by the stop).
    void disable_callbacks() { callbacks_enabled_ = false; }

private:
    struct Pending {
        Callback on_done;
        EventLoop::TimerId timer = 0;
    };

    RequestId start(const char* method, const std::string& url, const std::string& body, int timeout_ms,
                    Callback on_done);
    // Delivers the result of `id` once; later calls for the same id do nothing. Loop thread only.
    void finish(RequestId id, HttpResponse response);
    bool is_pending(RequestId id);
    static void on_event(struct mg_connection* c, int ev, void* ev_data);
    void log(LogLevel level, const std::string& message) const;

    EventLoop& loop_;
    ILogger* logger_;
    std::atomic<bool> callbacks_enabled_{true};
    std::atomic<RequestId> next_id_{1};

    std::mutex mutex_;
    std::condition_variable done_;
    std::map<RequestId, Pending> pending_;
    RequestId firing_ = 0;  // request whose callback is running

    std::map<unsigned long, std::string> unsent_;  // connection id -> request text; loop thread only
};

}  // namespace pal
