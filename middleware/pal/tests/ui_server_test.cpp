// Unit tests of PAL-WS (Section 5.3.3): TST-223 (IF-INT-01 – IF-INT-03, REQ-58, ER-40).
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_client.hpp"
#include "platform_fixture.hpp"

namespace {

using netclient::http_request;
using netclient::WsClient;
using std::chrono::milliseconds;
using testutil::wait_until;

// Events of the IUiChannel handlers, in their order.
struct UiEvents {
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, bool>> connections;
    std::vector<std::pair<std::uint64_t, std::string>> messages;
    void attach(pal::IUiChannel& ui) {
        ui.set_handlers(
            [this](std::uint64_t client, const std::string& text) {
                std::lock_guard<std::mutex> lock(mutex);
                messages.emplace_back(client, text);
            },
            [this](std::uint64_t client, bool connected) {
                std::lock_guard<std::mutex> lock(mutex);
                connections.emplace_back(client, connected);
            });
    }
    std::size_t connection_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return connections.size();
    }
    std::size_t message_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return messages.size();
    }
};

const std::string kPlaylist =
    "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:1.000,\nseg-0.ts\n";

// Step 1: the files of webui_dir and hls_dir with the content types of IF-INT-02; unknown -> 404.
TEST(Tst223, WebFilesAndHlsFilesWithTheirContentTypes) {
    testutil::TestPlatform t;
    t.write_web_file("index.html", "<!doctype html><title>M15</title>");
    t.write_web_file("css/style.css", "body { background: #000; }");
    t.write_web_file("js/main.js", "import { x } from './model.js';");
    t.write_web_file(".hidden", "secret");
    std::ofstream(t.dir.path() / "outside.txt") << "outside";
    pal::Platform& p = t.create();
    ASSERT_TRUE(p.start());
    const auto port = t.linux_platform().ui_port();
    ASSERT_NE(port, 0);
    ASSERT_TRUE(p.hls().open_session(1));
    const std::string segment(188 * 100, 'G');
    ASSERT_TRUE(p.hls().write_file(1, "index.m3u8", reinterpret_cast<const std::uint8_t*>(kPlaylist.data()), kPlaylist.size()));
    ASSERT_TRUE(p.hls().write_file(1, "seg-0.ts", reinterpret_cast<const std::uint8_t*>(segment.data()), segment.size()));

    auto r = http_request(port, "GET", "/");
    ASSERT_TRUE(r.answered);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "<!doctype html><title>M15</title>");
    EXPECT_EQ(r.headers["content-type"], "text/html; charset=utf-8");

    r = http_request(port, "GET", "/css/style.css");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "body { background: #000; }");
    EXPECT_EQ(r.headers["content-type"], "text/css; charset=utf-8");
    r = http_request(port, "GET", "/js/main.js");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.headers["content-type"], "text/javascript; charset=utf-8");  // ES modules need it

    r = http_request(port, "GET", "/hls/1/index.m3u8");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, kPlaylist);
    EXPECT_EQ(r.headers["content-type"], "application/vnd.apple.mpegurl");
    EXPECT_EQ(r.headers["cache-control"], "no-cache");
    r = http_request(port, "GET", "/hls/1/seg-0.ts");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, segment);
    EXPECT_EQ(r.headers["content-type"], "video/mp2t");
    EXPECT_EQ(r.headers.count("cache-control"), 0u);

    for (const std::string path : {"/hls/9/x.ts", "/hls/1/seg-1.ts", "/missing.html", "/hls/1/.tmp-1-seg-0.ts",
                                   "/hls/x/seg-0.ts", "/hls/1/seg-0.txt", "/../outside.txt", "/%2e%2e/outside.txt",
                                   "/hls/1/../../outside.txt", "/.hidden", "/css", "/hls/", "/ws/other"}) {
        r = http_request(port, "GET", path);
        EXPECT_TRUE(r.answered) << path;
        EXPECT_EQ(r.status, 404) << path;
        EXPECT_EQ(r.body.find("outside"), std::string::npos) << path;
    }
    r = http_request(port, "POST", "/");
    EXPECT_EQ(r.status, 404);
    r = http_request(port, "HEAD", "/hls/1/seg-0.ts");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.headers["content-length"], std::to_string(segment.size()));
    EXPECT_TRUE(r.body.empty());

    // hls.js keeps the connection open: several requests on one connection are all answered.
    const int fd = netclient::connect_local(port);
    std::string buffer;
    netclient::send_all(fd, "GET /hls/1/index.m3u8 HTTP/1.1\r\nHost: x\r\n\r\nGET /hls/1/seg-0.ts HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto first = netclient::read_reply(fd, buffer);
    const auto second = netclient::read_reply(fd, buffer);
    netclient::send_all(fd, "GET /css/style.css HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto third = netclient::read_reply(fd, buffer);
    close(fd);
    EXPECT_EQ(first.body, kPlaylist);
    EXPECT_EQ(second.body, segment);
    EXPECT_EQ(third.status, 200);
    p.stop();
}

// ER-40 and the error paths of Platform::start(): no index.html -> 404 for the page; a port in use ->
// start() returns false.
TEST(Tst223, MissingWebFilesGive404AndAPortInUseFailsTheStart) {
    testutil::TestPlatform empty;
    pal::Platform& p = empty.create();
    ASSERT_TRUE(p.start());
    const auto port = empty.linux_platform().ui_port();
    EXPECT_EQ(http_request(port, "GET", "/").status, 404);
    EXPECT_EQ(http_request(port, "GET", "/index.html").status, 404);

    testutil::TestPlatform second;
    second.config.ui_http_port = port;  // taken by the first platform
    pal::Platform& q = second.create();
    EXPECT_FALSE(q.start());
    EXPECT_NE(second.log_text().find("ERROR [PAL-WS] cannot listen on 127.0.0.1:" + std::to_string(port)), std::string::npos);
    EXPECT_FALSE(q.start());  // a failed platform is not started again
    q.stop();
    p.stop();
}

// Step 2: text frames both ways, send to one client, broadcast to two, a binary frame of random bytes
// is ignored, an abrupt disconnect is reported, and the server keeps running (REQ-58). The content
// of the frames is never logged (keys, REQ-55).
TEST(Tst223, WebSocketTextFramesBroadcastBinaryFrameAndAbruptDisconnect) {
    testutil::TestPlatform t;
    pal::Platform& p = t.create();
    UiEvents events;
    events.attach(p.ui());
    ASSERT_TRUE(p.start());
    const auto port = t.linux_platform().ui_port();

    WsClient a(port), b(port);
    ASSERT_TRUE(a.connected()) << a.handshake_status();
    ASSERT_TRUE(b.connected()) << b.handshake_status();
    ASSERT_TRUE(wait_until([&] { return events.connection_count() == 2; }));
    std::uint64_t id_a, id_b;
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        EXPECT_TRUE(events.connections[0].second);
        EXPECT_TRUE(events.connections[1].second);
        EXPECT_NE(events.connections[0].first, events.connections[1].first);
    }
    // Which id is which: each client sends one frame.
    ASSERT_TRUE(a.send_text(R"({"type":"command","id":1,"name":"get_status","params":{}})"));
    ASSERT_TRUE(wait_until([&] { return events.message_count() == 1; }));
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        id_a = events.messages[0].first;
        EXPECT_EQ(events.messages[0].second, R"({"type":"command","id":1,"name":"get_status","params":{}})");
    }
    ASSERT_TRUE(b.send_text(R"({"type":"command","id":7,"name":"set_key","params":{"key":"3A7F00112233445566778899AABBCCDD"}})"));
    ASSERT_TRUE(wait_until([&] { return events.message_count() == 2; }));
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        id_b = events.messages[1].first;
    }
    EXPECT_NE(id_a, id_b);

    p.ui().send(id_a, R"({"type":"response","id":1,"ok":true,"result":{}})");
    EXPECT_EQ(a.receive_text().value_or("<none>"), R"({"type":"response","id":1,"ok":true,"result":{}})");
    p.ui().broadcast(R"({"type":"event","name":"mw_status","data":{"reachable":true}})");
    EXPECT_EQ(a.receive_text().value_or("<none>"), R"({"type":"event","name":"mw_status","data":{"reachable":true}})");
    EXPECT_EQ(b.receive_text().value_or("<none>"), R"({"type":"event","name":"mw_status","data":{"reachable":true}})");
    EXPECT_FALSE(b.receive_text(300).has_value()) << "b received the frame sent to a";

    // A long message (over 64 KiB) arrives whole; frames keep their order.
    const std::string big(70000, 'x');
    p.ui().send(id_b, big);
    p.ui().send(id_b, "after");
    EXPECT_EQ(b.receive_text().value_or("<none>"), big);
    EXPECT_EQ(b.receive_text().value_or("<none>"), "after");

    std::mt19937 rng(7);
    std::string random(1000, '\0');
    for (char& c : random) c = static_cast<char>(rng());
    ASSERT_TRUE(b.send_frame(random, 2));  // binary: ignored
    ASSERT_TRUE(b.send_text("still open"));
    ASSERT_TRUE(wait_until([&] { return events.message_count() == 3; }));
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        EXPECT_EQ(events.messages[2], std::make_pair(id_b, std::string("still open")));
    }

    a.abort();  // no close frame
    ASSERT_TRUE(wait_until([&] { return events.connection_count() == 3; }));
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        EXPECT_EQ(events.connections[2], std::make_pair(id_a, false));
    }
    p.ui().send(id_a, "to a client that is gone");  // skipped
    WsClient c(port);  // the server keeps running
    EXPECT_TRUE(c.connected());
    ASSERT_TRUE(wait_until([&] { return events.connection_count() == 4; }));

    const std::string log = t.log_text();
    EXPECT_EQ(log.find("3A7F00112233445566778899AABBCCDD"), std::string::npos);
    EXPECT_EQ(log.find("get_status"), std::string::npos);
    EXPECT_NE(log.find("WARN [PAL-WS] binary frame from control client"), std::string::npos);
    p.stop();
}

// The local addresses of the open sockets of this process, from /proc/net/tcp or /proc/net/udp: "ADDR:PORT"
// in hexadecimal, as the kernel lists them (state 0A = LISTEN for TCP, 07 = unconnected for UDP).
std::vector<std::string> local_sockets(const std::string& table, const std::string& state) {
    std::ifstream in("/proc/net/" + table);
    std::vector<std::string> out;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string slot, local, remote, st;
        fields >> slot >> local >> remote >> st;
        if (st == state) out.push_back(local);
    }
    return out;
}

std::string hex_port(std::uint16_t port) {
    char text[8];
    std::snprintf(text, sizeof(text), "%04X", port);
    return text;
}

// Section 4.2.5 (HLR-20): PAL-WS listens on 127.0.0.1 only, and so does the tuner socket of HAL-TUN.
TEST(Tst223, ServerAndTunerListenOn127001Only) {
    testutil::TestPlatform t;
    pal::Platform& p = t.create();
    ASSERT_TRUE(p.start());
    ASSERT_TRUE(p.tuner().tune({0, 1180331842}));
    const std::string ui = hex_port(t.linux_platform().ui_port());
    const std::string rtp = hex_port(t.linux_platform().tuner_port());
    int ui_found = 0, rtp_found = 0;
    for (const std::string& s : local_sockets("tcp", "0A")) {
        if (s.size() > 5 && s.substr(s.size() - 4) == ui) {
            ++ui_found;
            EXPECT_EQ(s, "0100007F:" + ui);  // 127.0.0.1 in the kernel's byte order
        }
    }
    for (const std::string& s : local_sockets("udp", "07")) {
        if (s.size() > 5 && s.substr(s.size() - 4) == rtp) {
            ++rtp_found;
            EXPECT_EQ(s, "0100007F:" + rtp);
        }
    }
    EXPECT_EQ(ui_found, 1);
    EXPECT_EQ(rtp_found, 1);
    p.stop();
}

// REQ-58 on the HTTP port: random bytes, a request without the WebSocket headers on /ws/control and a
// client that closes in the middle of a request do not stop the server.
TEST(Tst223, InvalidInputOnTheHttpPortDoesNotStopTheServer) {
    testutil::TestPlatform t;
    t.write_web_file("index.html", "ok");
    pal::Platform& p = t.create();
    ASSERT_TRUE(p.start());
    const auto port = t.linux_platform().ui_port();
    std::mt19937 rng(11);
    for (int i = 0; i < 20; ++i) {
        const int fd = netclient::connect_local(port);
        std::string junk(4096, '\0');
        for (char& c : junk) c = static_cast<char>(rng());
        netclient::send_all(fd, junk);
        close(fd);
    }
    const int half = netclient::connect_local(port);
    netclient::send_all(half, "GET /index.html HTTP/1.1\r\nHo");
    close(half);
    EXPECT_EQ(http_request(port, "GET", "/ws/control").status, 426);
    const auto r = http_request(port, "GET", "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "ok");
    p.stop();
}

}  // namespace
