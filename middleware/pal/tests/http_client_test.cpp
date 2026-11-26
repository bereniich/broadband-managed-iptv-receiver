// Unit tests of PAL-HTTP (Section 5.3.3): TST-222, against a local test HTTP server (Mongoose on
// port 0, its own manager and thread).
#include <gtest/gtest.h>

#include <cstdio>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mongoose.h"
#include "net_client.hpp"
#include "platform_fixture.hpp"

namespace {

using pal::HttpResponse;
using std::chrono::milliseconds;
using std::chrono::steady_clock;
using testutil::wait_until;

// Routes: /echo answers 200 with "method=… type=… body=…"; /missing answers 404 "nope"; /slow answers
// 200 "late" after 1 000 ms.
class TestServer {
public:
    TestServer() {
        mg_log_set(MG_LL_ERROR);  // process-wide; the PAL sets the same level
        mg_mgr_init(&mgr_);
        mg_connection* c = mg_http_listen(&mgr_, "http://127.0.0.1:0", &TestServer::on_event, this);
        port_ = c != nullptr ? mg_ntohs(c->loc.port) : 0;
        thread_ = std::thread([this] {
            while (running_) mg_mgr_poll(&mgr_, 5);
        });
    }
    ~TestServer() {
        running_ = false;
        thread_.join();
        mg_mgr_free(&mgr_);
    }
    std::uint16_t port() const { return port_; }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    int requests() const { return requests_; }

private:
    static void on_event(mg_connection* c, int ev, void* ev_data) {
        auto* self = static_cast<TestServer*>(c->fn_data);
        if (ev == MG_EV_HTTP_MSG) {
            ++self->requests_;
            auto* hm = static_cast<mg_http_message*>(ev_data);
            const std::string uri(hm->uri.buf, hm->uri.len);
            if (uri == "/echo") {
                const mg_str* type = mg_http_get_header(hm, "Content-Type");
                mg_http_reply(c, 200, "", "method=%.*s type=%.*s body=%.*s", (int)hm->method.len, hm->method.buf,
                              type != nullptr ? (int)type->len : 0, type != nullptr ? type->buf : "",
                              (int)hm->body.len, hm->body.buf);
            } else if (uri == "/slow") {
                self->due_[c->id] = steady_clock::now() + milliseconds(1000);
            } else {
                mg_http_reply(c, 404, "", "nope");
            }
        } else if (ev == MG_EV_POLL) {
            const auto it = self->due_.find(c->id);
            if (it != self->due_.end() && steady_clock::now() >= it->second) {
                self->due_.erase(it);
                mg_http_reply(c, 200, "", "late");
            }
        } else if (ev == MG_EV_CLOSE) {
            self->due_.erase(c->id);
        }
    }

    mg_mgr mgr_{};
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{true};
    std::atomic<int> requests_{0};
    std::map<unsigned long, steady_clock::time_point> due_;  // server thread only
    std::thread thread_;
};

struct Result {
    std::atomic<int> calls{0};
    HttpResponse response;
    steady_clock::time_point at{};
    std::thread::id thread{};
};

pal::IHttpClient::Callback record(Result& r) {
    return [&r](const HttpResponse& response) {
        r.response = response;
        r.at = steady_clock::now();
        r.thread = std::this_thread::get_id();
        ++r.calls;
    };
}

// Step 1: get and post_json give one callback with status and body, on the event-loop thread; an
// HTTP error status is a transported response.
TEST(Tst222, GetAndPostJsonGiveOneCallbackWithStatusAndBody) {
    TestServer server;
    ASSERT_NE(server.port(), 0);
    testutil::TestPlatform t;
    pal::Platform& p = t.create();
    ASSERT_TRUE(p.start());

    Result get, post, missing;
    p.http().get(server.url("/echo"), 1000, record(get));
    p.http().post_json(server.url("/echo"), R"({"service_id":"svc-1","start":"live"})", 1000, record(post));
    p.http().get(server.url("/missing"), 1000, record(missing));
    ASSERT_TRUE(wait_until([&] { return get.calls == 1 && post.calls == 1 && missing.calls == 1; }));
    std::this_thread::sleep_for(milliseconds(200));  // exactly once
    EXPECT_EQ(get.calls.load(), 1);
    EXPECT_EQ(post.calls.load(), 1);
    EXPECT_EQ(missing.calls.load(), 1);

    EXPECT_TRUE(get.response.transport_ok);
    EXPECT_EQ(get.response.status, 200);
    EXPECT_EQ(get.response.body, "method=GET type= body=");
    EXPECT_TRUE(post.response.transport_ok);
    EXPECT_EQ(post.response.status, 200);
    EXPECT_EQ(post.response.body, R"(method=POST type=application/json body={"service_id":"svc-1","start":"live"})");
    EXPECT_TRUE(missing.response.transport_ok);
    EXPECT_EQ(missing.response.status, 404);
    EXPECT_EQ(missing.response.body, "nope");
    EXPECT_NE(get.thread, std::this_thread::get_id());
    EXPECT_EQ(get.thread, post.thread);
    p.stop();
}

// Step 2: a closed port gives transport_ok = false (status 0, an error text); so do URLs the client
// does not support. The callback never runs inside the call.
TEST(Tst222, ClosedPortAndUnsupportedUrlsGiveTransportFailure) {
    testutil::TestPlatform t;
    pal::Platform& p = t.create();
    ASSERT_TRUE(p.start());
    const std::uint16_t closed = netclient::free_tcp_port();
    std::map<std::string, Result> results;
    const std::vector<std::string> urls = {"http://127.0.0.1:" + std::to_string(closed) + "/api/v1/services",
                                           "https://127.0.0.1:8080/x", "http://localhost:8080/x", "nonsense",
                                           "http://127.0.0.1:0/x"};
    for (const std::string& url : urls) results[url];
    for (const std::string& url : urls) p.http().get(url, 1000, record(results[url]));
    ASSERT_TRUE(wait_until([&] {
        for (auto& r : results) {
            if (r.second.calls != 1) return false;
        }
        return true;
    }));
    for (auto& r : results) {
        EXPECT_FALSE(r.second.response.transport_ok) << r.first;
        EXPECT_EQ(r.second.response.status, 0) << r.first;
        EXPECT_FALSE(r.second.response.error.empty()) << r.first;
        EXPECT_NE(r.second.thread, std::this_thread::get_id()) << r.first;
    }
    EXPECT_EQ(results["http://localhost:8080/x"].response.error, "the host must be an IPv4 address");
    p.stop();
}

// Step 3: the server delays 1 s, the timeout is 300 ms: transport_ok = false at 300 ms ± 50 ms, and no
// second callback when the late answer would arrive.
TEST(Tst222, TimeoutOf300msWithAServerThatAnswersAfter1s) {
    TestServer server;
    testutil::TestPlatform t;
    pal::Platform& p = t.create();
    ASSERT_TRUE(p.start());
    Result r;
    const auto started = steady_clock::now();
    p.http().get(server.url("/slow"), 300, record(r));
    ASSERT_TRUE(wait_until([&] { return r.calls == 1; }));
    const double elapsed = std::chrono::duration<double, std::milli>(r.at - started).count();
    EXPECT_FALSE(r.response.transport_ok);
    EXPECT_EQ(r.response.status, 0);
    std::printf("TST-222 measured: timeout callback after %.1f ms (limit 300 +/- 50 ms)\n", elapsed);
    EXPECT_GE(elapsed, 250);
    EXPECT_LE(elapsed, 350);
    std::this_thread::sleep_for(milliseconds(1200));
    EXPECT_EQ(r.calls.load(), 1);
    p.stop();
}

// Step 4: cancel before the answer: no callback. cancel() waits for a callback that is running on the
// event loop, and works from inside a callback; cancelling an unknown id does nothing.
TEST(Tst222, CancelBeforeTheAnswerGivesNoCallback) {
    TestServer server;
    testutil::TestPlatform t;
    pal::Platform& p = t.create();
    ASSERT_TRUE(p.start());

    Result cancelled;
    const auto id = p.http().get(server.url("/slow"), 3000, record(cancelled));
    ASSERT_TRUE(wait_until([&] { return server.requests() == 1; }));
    p.http().cancel(id);
    std::this_thread::sleep_for(milliseconds(1500));  // the server has answered by now
    EXPECT_EQ(cancelled.calls.load(), 0);
    p.http().cancel(id);      // again: nothing
    p.http().cancel(987654);  // unknown: nothing

    std::atomic<bool> entered{false}, finished{false};
    const auto slow_cb = p.http().get(server.url("/echo"), 1000, [&](const HttpResponse&) {
        entered = true;
        std::this_thread::sleep_for(milliseconds(150));
        finished = true;
    });
    ASSERT_TRUE(wait_until([&] { return entered.load(); }));
    p.http().cancel(slow_cb);
    EXPECT_TRUE(finished.load()) << "cancel returned while the callback was running";

    Result other;
    const auto other_id = p.http().get(server.url("/slow"), 3000, record(other));
    p.http().get(server.url("/echo"), 1000, [&](const HttpResponse&) { p.http().cancel(other_id); });
    std::this_thread::sleep_for(milliseconds(1300));
    EXPECT_EQ(other.calls.load(), 0);
    p.stop();
}

// Rule 2 of Section 3.9.1: requests from several threads at once; every request gets exactly one
// callback.
TEST(Tst222, FiftyConcurrentRequestsGetOneCallbackEach) {
    TestServer server;
    testutil::TestPlatform t;
    pal::Platform& p = t.create();
    ASSERT_TRUE(p.start());
    std::vector<Result> results(50);
    std::vector<std::thread> threads;
    for (int k = 0; k < 5; ++k) {
        threads.emplace_back([&, k] {
            for (int i = 0; i < 10; ++i) {
                Result& r = results[static_cast<std::size_t>(k * 10 + i)];
                if (i % 2 == 0) {
                    p.http().get(server.url("/echo"), 2000, record(r));
                } else {
                    p.http().post_json(server.url("/echo"), "{\"n\":" + std::to_string(k * 10 + i) + "}", 2000, record(r));
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_TRUE(wait_until([&] {
        for (auto& r : results) {
            if (r.calls != 1) return false;
        }
        return true;
    }));
    std::this_thread::sleep_for(milliseconds(100));
    for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(results[i].calls.load(), 1);
        EXPECT_EQ(results[i].response.status, 200);
        if (i % 2 == 1) {
            EXPECT_NE(results[i].response.body.find("{\"n\":" + std::to_string(i) + "}"), std::string::npos);
        }
    }
    p.stop();
}

}  // namespace
