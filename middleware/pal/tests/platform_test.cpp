// Unit tests of the platform as a whole (Section 5.3.3): TST-226 (Section 3.9.1, rule 4 for
// Platform::stop) and the factory create_platform.
#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "mongoose.h"
#include "net_client.hpp"
#include "platform_fixture.hpp"
#include "ts_helper.hpp"

namespace {

using pal::FilterType;
using std::chrono::milliseconds;
using testhelp::Packet;
using testutil::wait_until;

constexpr std::uint32_t kSsrc = 1180331842;

// A slow HTTP server for a request that is still pending at the stop.
class SilentServer {
public:
    SilentServer() {
        mg_log_set(MG_LL_ERROR);
        mg_mgr_init(&mgr_);
        mg_connection* c = mg_http_listen(&mgr_, "http://127.0.0.1:0", &SilentServer::on_event, nullptr);
        port_ = c != nullptr ? mg_ntohs(c->loc.port) : 0;
        thread_ = std::thread([this] {
            while (running_) mg_mgr_poll(&mgr_, 5);
        });
    }
    ~SilentServer() {
        running_ = false;
        thread_.join();
        mg_mgr_free(&mgr_);
    }
    std::uint16_t port() const { return port_; }

private:
    static void on_event(mg_connection*, int, void*) {}  // never answers

    mg_mgr mgr_{};
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

// TST-226: a platform with the tuner receiving, a TS filter delivering, timers running, a WebSocket
// client connected and sending, and an HTTP request pending. After Platform::stop() returns, no
// callback of any interface is called; later tune(), timers, requests and frames have no effect.
TEST(Tst226, NoCallbackOfAnyInterfaceAfterPlatformStop) {
    SilentServer silent;
    testutil::TestPlatform t;
    pal::Platform& p = t.create();
    std::atomic<bool> stopped{false};
    std::atomic<int> after_stop{0};
    std::atomic<int> data{0}, statuses{0}, messages{0}, connections{0}, timers{0}, http{0};
    auto check = [&](std::atomic<int>& counter) {
        if (stopped) ++after_stop;
        ++counter;
    };
    p.tuner().set_status_handler([&](pal::TunerStatus) { check(statuses); });
    p.ui().set_handlers([&](std::uint64_t, const std::string&) { check(messages); },
                        [&](std::uint64_t, bool) { check(connections); });
    ASSERT_TRUE(p.start());
    ASSERT_TRUE(p.tuner().tune({0, kSsrc}));
    const auto filter = p.demux().open_filter({FilterType::Ts, 256}, [&](const pal::IDemux::FilterData&) { check(data); });
    ASSERT_TRUE(p.demux().start_filter(filter));

    std::function<void()> rearm = [&] {
        check(timers);
        p.clock().start_timer(2, rearm);
    };
    p.clock().start_timer(2, rearm);
    p.http().get("http://127.0.0.1:" + std::to_string(silent.port()) + "/slow", 5000,
                 [&](const pal::HttpResponse&) { check(http); });

    std::atomic<bool> running{true};
    const std::uint16_t rtp_port = t.linux_platform().tuner_port();
    std::thread rtp([&] {
        testhelp::UdpSender sender;
        std::uint8_t cc = 0;
        std::uint16_t seq = 0;
        while (running) {
            std::vector<Packet> block;
            for (int k = 0; k < 7; ++k) block.push_back(testhelp::packet(256, cc++, false));
            testhelp::RtpHeader h;
            h.sequence = seq++;
            h.ssrc = kSsrc;
            sender.send(rtp_port, testhelp::rtp_datagram(h, block));
            std::this_thread::sleep_for(milliseconds(2));
        }
    });
    netclient::WsClient client(t.linux_platform().ui_port());
    ASSERT_TRUE(client.connected());
    std::thread ws([&] {
        while (running && client.send_text("ping")) std::this_thread::sleep_for(milliseconds(2));
    });
    ASSERT_TRUE(wait_until([&] { return data > 100 && statuses >= 1 && messages > 10 && timers > 10 && connections == 1; }));

    p.stop();
    stopped = true;
    std::this_thread::sleep_for(milliseconds(500));  // RTP, frames and timers would continue here

    EXPECT_FALSE(p.tuner().tune({0, kSsrc}));  // refused after the stop
    p.clock().start_timer(1, [&] { check(timers); });
    p.http().get("http://127.0.0.1:1/x", 100, [&](const pal::HttpResponse&) { check(http); });
    p.ui().broadcast("after stop");
    std::this_thread::sleep_for(milliseconds(300));
    running = false;
    rtp.join();
    ws.join();

    EXPECT_EQ(after_stop.load(), 0);
    EXPECT_EQ(http.load(), 0);           // the pending request is never answered
    EXPECT_EQ(connections.load(), 1);    // the connection closed by the stop is not reported either
    EXPECT_TRUE(client.wait_closed());   // the server side is closed
    EXPECT_FALSE(p.start());             // a stopped platform is not started again
    p.stop();                            // idempotent
    EXPECT_NE(t.log_text().find("INFO [PAL] platform stopped"), std::string::npos);
}

// The factory: the log file of the configuration is created with its directory; if it cannot be
// opened, create_platform returns nullptr (AL-MAIN then exits with code 1).
TEST(Tst226, CreatePlatformOpensTheLogFileOrReturnsNull) {
    testutil::TestPlatform t;
    t.config.log_file = (t.dir.path() / "deep" / "logs" / "middleware.log").string();
    pal::Platform& p = t.create();
    p.logger().log(pal::LogLevel::Info, "AL-MAIN", "started");
    EXPECT_NE(t.log_text().find("INFO [AL-MAIN] started"), std::string::npos);

    std::ofstream(t.dir.path() / "plain-file") << "x";
    pal::PlatformConfig bad = t.config;
    bad.log_file = (t.dir.path() / "plain-file" / "middleware.log").string();
    EXPECT_EQ(pal::create_platform(bad), nullptr);
}

}  // namespace
