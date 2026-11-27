// Unit tests of the head-end (Section 5.3.4): TST-311 – TST-317, and the head-end parts of TST-318
// and TST-320. Content: content/svc-1.ts … svc-4.ts. The head-end tests use the clear files of the
// protected services (svc-3.ts, svc-4.ts) in place of the encrypted ones: SRV-ARC and SRV-RTP read
// only the clear parts of a packet (PAT, PMT, adaptation field with the PCR), so the result is the
// same, and the tests do not depend on the packaging tool of WP-APP.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#include "archive.hpp"
#include "config.hpp"
#include "head_end.hpp"
#include "http.hpp"
#include "log.hpp"
#include "rtp_sender.hpp"
#include "services.hpp"
#include "test_support.hpp"

namespace {

using nlohmann::json;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

const std::string kRepo = M15_REPO_DIR;
const std::string kHeadEndBin = M15_HEADEND_BIN;
constexpr int64_t kT = 1795000000;      // worked example of Section 5.2.4: kT mod 600 = 400
constexpr int64_t kCycle0 = 1794999600;  // kT - (kT mod 600): content position 0
constexpr double kMuxRate = 3500000.0;   // bit/s (Section 4.7.2)
constexpr double kMaxPcrInterval = 0.0211;  // largest PCR interval of the content, s (FFmpeg: 20 ms)

std::string content_file(const std::string& name) { return kRepo + "/content/" + name; }

// ---- The test's own TS reader (independent of SRV-ARC) ----------------------------------------

uint16_t pid_of(const uint8_t* p) { return static_cast<uint16_t>(((p[1] & 0x1F) << 8) | p[2]); }

int64_t pcr_of(const uint8_t* p) {
    if ((p[3] & 0x20) == 0 || p[4] == 0 || (p[5] & 0x10) == 0) return -1;
    return (int64_t{p[6]} << 25) | (int64_t{p[7]} << 17) | (int64_t{p[8]} << 9) | (int64_t{p[9]} << 1) | (p[10] >> 7);
}

struct FileInfo {
    srv::TsPacket pat{}, pmt{};
    uint16_t pmt_pid = 0, pcr_pid = 0;
    int64_t first_pcr = -1;
    int64_t size = 0;
};

// First PAT, first PMT and first PCR of a content file.
FileInfo scan_file(const std::string& path) {
    FileInfo info;
    std::ifstream in(path, std::ios::binary);
    info.size = static_cast<int64_t>(std::filesystem::file_size(path));
    srv::TsPacket p{};
    bool pat = false, pmt = false;
    while (in.read(reinterpret_cast<char*>(p.data()), 188) && info.first_pcr < 0) {
        const uint16_t pid = pid_of(p.data());
        const bool pusi = (p[1] & 0x40) != 0;
        const int ps = (p[3] & 0x20) ? 5 + p[4] : 4;
        if (!pat && pid == 0 && pusi) {
            const int sec = ps + 1 + p[ps];
            info.pmt_pid = static_cast<uint16_t>(((p[sec + 10] & 0x1F) << 8) | p[sec + 11]);  // first program
            info.pat = p;
            pat = true;
        } else if (pat && !pmt && pid == info.pmt_pid && pusi) {
            const int sec = ps + 1 + p[ps];
            info.pcr_pid = static_cast<uint16_t>(((p[sec + 8] & 0x1F) << 8) | p[sec + 9]);
            info.pmt = p;
            pmt = true;
        } else if (pmt && pid == info.pcr_pid && pcr_of(p.data()) >= 0) {
            info.first_pcr = pcr_of(p.data());
        }
    }
    return info;
}

std::vector<uint8_t> read_bytes(const std::string& path, int64_t offset, size_t n) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(offset);
    std::vector<uint8_t> data(n);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(n));
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}

// ---- Content and archives -----------------------------------------------------------------------

// Services of content/services.json with the clear files for the protected services.
srv::ServiceCatalog test_catalog() {
    srv::ServiceCatalog catalog = srv::load_services(kRepo + "/content/services.json");
    for (srv::Service& s : catalog.services) {
        if (s.file.find(".enc.ts") != std::string::npos) s.file = s.service_id + ".ts";
    }
    return catalog;
}

// Archive of a content file, built once per test run (≈ 262 MB read per file).
std::shared_ptr<const srv::Archive> archive_of(const srv::Service& service) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const srv::Archive>> cache;
    const std::string path = content_file(service.file);
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.count(path)) return cache[path];
    if (!std::filesystem::exists(path)) {
        ADD_FAILURE() << "content missing: run content/generate.sh (" << path << ")";
        return nullptr;
    }
    return cache[path] = srv::Archive::load(service, path);
}

std::map<std::string, std::shared_ptr<const srv::Archive>> archives_of(const srv::ServiceCatalog& catalog) {
    std::map<std::string, std::shared_ptr<const srv::Archive>> out;
    for (const srv::Service& s : catalog.services) {
        auto a = archive_of(s);
        if (!a) return {};
        out[s.service_id] = a;
    }
    return out;
}

const srv::Service& service(const srv::ServiceCatalog& catalog, const std::string& id) {
    return *catalog.find(id);
}

// ---- RTP receiver --------------------------------------------------------------------------------

struct Datagram {
    steady_clock::time_point at;
    std::vector<uint8_t> data;

    uint16_t seq() const { return static_cast<uint16_t>((data[2] << 8) | data[3]); }
    uint32_t ts() const {
        return (uint32_t{data[4]} << 24) | (uint32_t{data[5]} << 16) | (uint32_t{data[6]} << 8) | data[7];
    }
    uint32_t ssrc() const {
        return (uint32_t{data[8]} << 24) | (uint32_t{data[9]} << 16) | (uint32_t{data[10]} << 8) | data[11];
    }
    size_t packets() const { return (data.size() - 12) / 188; }
    const uint8_t* packet(size_t i) const { return data.data() + 12 + 188 * i; }
};

class UdpReceiver {
public:
    UdpReceiver() {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        const int size = 8 << 20;
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        timeval tv{0, 20000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~UdpReceiver() { close(fd_); }

    int port() const { return port_; }

    std::vector<Datagram> receive_for(steady_clock::duration duration) {
        std::vector<Datagram> out;
        const auto deadline = steady_clock::now() + duration;
        uint8_t buf[2048];
        while (steady_clock::now() < deadline) {
            const ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n > 0) out.push_back({steady_clock::now(), std::vector<uint8_t>(buf, buf + n)});
        }
        return out;
    }

    // Everything already queued (loopback delivers in sendto, so this is all sent so far).
    std::vector<Datagram> drain() {
        std::vector<Datagram> out;
        uint8_t buf[2048];
        for (;;) {
            const ssize_t n = recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) break;
            out.push_back({steady_clock::now(), std::vector<uint8_t>(buf, buf + n)});
        }
        return out;
    }

private:
    int fd_ = -1;
    int port_ = 0;
};

int udp_send_socket() { return socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); }

// ---- Head-end in-process with an HTTP server on an ephemeral port ------------------------------

std::string post_request(const std::string& path, const std::string& body) {
    return "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

struct HeadEndUnderTest {
    std::unique_ptr<srv::HeadEnd> head_end;
    std::unique_ptr<srv::HttpServer> server;

    test::HttpReply post(const std::string& endpoint, const json& body) const {
        return test::http_exchange(server->port(), post_request("/api/v1/stream/" + endpoint, body.dump()));
    }
    ~HeadEndUnderTest() {
        if (server) server->stop();
        if (head_end) head_end->shutdown();
    }
};

std::unique_ptr<HeadEndUnderTest> start_head_end(srv::HeadEndOptions options, srv::Logger* logger = nullptr) {
    srv::ServiceCatalog catalog = test_catalog();
    auto archives = archives_of(catalog);
    if (archives.empty()) return nullptr;
    auto t = std::make_unique<HeadEndUnderTest>();
    t->head_end = std::make_unique<srv::HeadEnd>(std::move(catalog), std::move(archives), std::move(options), logger);
    srv::HeadEnd* he = t->head_end.get();
    t->server = std::make_unique<srv::HttpServer>([he](const srv::HttpRequest& r) { return he->handle(r); });
    std::string error;
    if (!t->server->start(0, &error)) {
        ADD_FAILURE() << error;
        return nullptr;
    }
    return t;
}

srv::WallClock fixed_wall_clock(int64_t t) {
    return [t] { return srv::from_unix_seconds(t); };
}

void expect_error(const test::HttpReply& reply, int status, const std::string& code) {
    ASSERT_TRUE(reply.answered);
    EXPECT_EQ(reply.status, status) << reply.body;
    const json body = json::parse(reply.body, nullptr, false);
    ASSERT_TRUE(body.is_object() && body.contains("error")) << reply.body;
    EXPECT_TRUE(body["error"]["message"].is_string()) << reply.body;
    if (!code.empty()) {
        EXPECT_EQ(body["error"]["code"], code) << reply.body;
    }
}

json start_body(const std::string& service_id, const json& start, int port, uint32_t ssrc) {
    return json{{"service_id", service_id}, {"start", start}, {"client_host", "127.0.0.1"},
                {"client_port", port}, {"ssrc", ssrc}};
}

// Copy of `body` with one field replaced.
json with(json body, const char* key, const json& value) {
    body[key] = value;
    return body;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// TST-311 — SRV-ARC: time -> position and cached PSI (REQ-42, Section 4.7.4)
// ---------------------------------------------------------------------------------------------

TEST(Tst311, PositionIsPcrPacketAtMostOnePcrIntervalBeforeP) {
    const srv::ServiceCatalog catalog = test_catalog();
    const auto archive = archive_of(service(catalog, "svc-1"));
    ASSERT_TRUE(archive);
    const std::string path = content_file("svc-1.ts");
    const FileInfo info = scan_file(path);

    EXPECT_EQ(archive->pat(), info.pat);  // the first PAT and PMT packets of the file
    EXPECT_EQ(archive->pmt(), info.pmt);
    EXPECT_EQ(archive->pmt_pid(), 4096);
    EXPECT_EQ(archive->pcr_pid(), 256);
    EXPECT_EQ(archive->duration_s(), 600);
    EXPECT_NEAR(archive->measured_duration_s(), 600.0, 0.1);

    for (const int64_t p : {0, 1, 299, 300, 599}) {
        SCOPED_TRACE("T mod 600 = " + std::to_string(p));
        const srv::Archive::Position pos = archive->position(kCycle0 + p);  // fixed time T
        EXPECT_EQ(pos.offset % 188, 0);
        const std::vector<uint8_t> packet = read_bytes(path, pos.offset, 188);
        ASSERT_EQ(packet.size(), 188u);
        EXPECT_EQ(packet[0], 0x47);
        EXPECT_EQ(pid_of(packet.data()), 256);
        const int64_t pcr = pcr_of(packet.data());
        ASSERT_GE(pcr, 0) << "no PCR at the returned offset";
        const double c = static_cast<double>(pcr - info.first_pcr) / 90000.0;
        std::printf("TST-311 measured: T mod 600 = %lld s: p - c = %.4f s (limit 0 <= p - c < 0.04 s)\n",
                    static_cast<long long>(p), p - c);
        EXPECT_GE(p - c, 0.0);
        EXPECT_LT(p - c, 0.04);
        EXPECT_EQ(pos.c, pcr - info.first_pcr);
    }
    // The position depends only on T mod 600 (D-08).
    EXPECT_EQ(archive->position(kCycle0 + 123).offset, archive->position(kCycle0 + 123 - 7 * 600).offset);
}

// ---------------------------------------------------------------------------------------------
// TST-312 — archive window (REQ-42, Section 3.1)
// ---------------------------------------------------------------------------------------------

TEST(Tst312, ArchiveWindowBoundaries) {
    const int64_t t = kT;
    EXPECT_EQ(srv::check_archive_window(t - 7200, t, 7200), srv::WindowCheck::Ok);
    EXPECT_EQ(srv::check_archive_window(t - 7201, t, 7200), srv::WindowCheck::OutOfArchive);
    EXPECT_EQ(srv::check_archive_window(t - 1, t, 7200), srv::WindowCheck::Ok);
    EXPECT_EQ(srv::check_archive_window(t, t, 7200), srv::WindowCheck::FutureTime);
}

// ---------------------------------------------------------------------------------------------
// TST-313 — missing content file, duration mismatch (ER-38)
// ---------------------------------------------------------------------------------------------

namespace {

std::string archive_error(const srv::Service& s, const std::string& path) {
    try {
        srv::Archive::load(s, path);
    } catch (const srv::ArchiveError& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST(Tst313, ArchiveRefusesMissingOrInvalidContentNamingServiceAndFile) {
    srv::Service s = test_catalog().services[0];  // svc-1, 2 x 300 s
    test::TempDir dir;

    // IN-11: missing content file; programs sum 590 s.
    std::string msg = archive_error(s, dir.file("missing.ts"));
    EXPECT_NE(msg.find("svc-1"), std::string::npos) << msg;
    EXPECT_NE(msg.find(dir.file("missing.ts")), std::string::npos) << msg;
    srv::Service short_service = s;
    short_service.programs[0].duration = 290;
    ASSERT_TRUE(std::filesystem::exists(content_file("svc-1.ts"))) << "content missing: run content/generate.sh";
    msg = archive_error(short_service, content_file("svc-1.ts"));
    EXPECT_NE(msg.find("svc-1"), std::string::npos) << msg;
    EXPECT_NE(msg.find(content_file("svc-1.ts")), std::string::npos) << msg;
    EXPECT_NE(msg.find("599.9"), std::string::npos) << msg;  // the measured duration

    // Further invalid files: not whole packets, no sync byte, no PAT, no PCR.
    std::vector<uint8_t> null_packet(188, 0xFF);
    null_packet[0] = 0x47;
    null_packet[1] = 0x1F;
    null_packet[2] = 0xFF;
    null_packet[3] = 0x10;
    test::write_text(dir.file("short.ts"), std::string(100, '\x47'));
    test::write_text(dir.file("nosync.ts"), std::string(188 * 3, '\0'));
    test::write_text(dir.file("nopat.ts"), std::string(null_packet.begin(), null_packet.end()) +
                                               std::string(null_packet.begin(), null_packet.end()));
    const std::vector<uint8_t> head = read_bytes(content_file("svc-1.ts"), 0, 3 * 188);  // SDT, PAT, PMT
    test::write_text(dir.file("nopcr.ts"), std::string(head.begin(), head.end()));
    for (const char* name : {"short.ts", "nosync.ts", "nopat.ts", "nopcr.ts"}) {
        msg = archive_error(s, dir.file(name));
        EXPECT_NE(msg.find("svc-1: " + dir.file(name) + ": "), std::string::npos) << name << ": " << msg;
    }
}

namespace {

std::pair<int, std::string> run_head_end(const test::TempDir& dir, const std::string& config) {
    const std::string out = dir.file("output.txt");
    test::Process p({kHeadEndBin, "--config", config}, out);
    EXPECT_TRUE(p.started());
    const int code = p.wait(seconds(30));
    return {code, test::read_text(out)};
}

std::string head_end_config(const test::TempDir& dir, const std::string& services_file, int port) {
    json c;
    c["headend_port"] = port;
    c["services_file"] = services_file;
    c["content_dir"] = kRepo + "/content";
    c["log_dir"] = dir.file("logs");
    return c.dump();
}

}  // namespace

TEST(Tst313, HeadEndExitsWithCode1NamingServiceAndFile) {
    test::TempDir dir;
    const int port = test::free_tcp_port();
    json services = json::parse(test::read_text(kRepo + "/content/services.json"));
    services["services"] = json::array({services["services"][0]});  // svc-1 only

    json missing = services;
    missing["services"][0]["file"] = "no-such-file.ts";
    test::write_text(dir.file("missing.json"), missing.dump());
    test::write_text(dir.file("cfg-missing.json"), head_end_config(dir, dir.file("missing.json"), port));
    auto [code, output] = run_head_end(dir, dir.file("cfg-missing.json"));
    EXPECT_EQ(code, 1) << output;
    EXPECT_NE(output.find("svc-1"), std::string::npos) << output;
    EXPECT_NE(output.find("no-such-file.ts"), std::string::npos) << output;

    json sum590 = services;
    sum590["services"][0]["programs"][0]["duration"] = 290;
    test::write_text(dir.file("sum590.json"), sum590.dump());
    test::write_text(dir.file("cfg-sum590.json"), head_end_config(dir, dir.file("sum590.json"), port));
    std::tie(code, output) = run_head_end(dir, dir.file("cfg-sum590.json"));
    EXPECT_EQ(code, 1) << output;
    EXPECT_NE(output.find("svc-1"), std::string::npos) << output;
    EXPECT_NE(output.find("svc-1.ts"), std::string::npos) << output;
}

// ---------------------------------------------------------------------------------------------
// TST-314 — SRV-RTP: datagrams, header, loop boundary, bitrate (REQ-66, IF-EXT-05, B10)
// ---------------------------------------------------------------------------------------------

TEST(Tst314, SenderStreamsAcrossTheLoopBoundaryInRealTime) {
    const srv::ServiceCatalog catalog = test_catalog();
    const auto archive = archive_of(service(catalog, "svc-1"));
    ASSERT_TRUE(archive);
    const std::string path = content_file("svc-1.ts");
    const FileInfo info = scan_file(path);
    UdpReceiver rx;
    const int send_fd = udp_send_socket();

    srv::RtpSessionParams params;
    params.session_id = "hs-test314";
    params.client_host = "127.0.0.1";
    params.client_port = static_cast<uint16_t>(rx.port());
    params.ssrc = 0xC0FFEE01;
    params.first_sequence = 65000;      // wraps during the test
    params.timestamp_base = 0xFFF00000;  // wraps during the test
    const srv::Archive::Position start = archive->position(kCycle0 + 597);  // T mod 600 = 597
    srv::RtpSender sender(archive, send_fd, params, start);
    sender.start();
    const std::vector<Datagram> d = rx.receive_for(seconds(12));
    sender.stop();
    close(send_fd);
    ASSERT_GT(d.size(), 3000u);

    // Header of every datagram; payload of whole TS packets.
    for (size_t i = 0; i < d.size(); ++i) {
        ASSERT_GE(d[i].data.size(), 12u + 188u) << i;
        ASSERT_EQ((d[i].data.size() - 12) % 188, 0u) << i;
        EXPECT_EQ(d[i].data[0], 0x80) << i;  // V = 2, P = 0, X = 0, CC = 0
        EXPECT_EQ(d[i].data[1], 33) << i;    // M = 0, PT = 33
        EXPECT_EQ(d[i].ssrc(), params.ssrc) << i;
        for (size_t k = 0; k < d[i].packets(); ++k) ASSERT_EQ(d[i].packet(k)[0], 0x47) << i;
        if (i > 0) {
            EXPECT_EQ(static_cast<uint16_t>(d[i - 1].seq() + 1), d[i].seq()) << "sequence gap at " << i;
            EXPECT_GE(static_cast<int32_t>(d[i].ts() - d[i - 1].ts()), 0) << "timestamp decreases at " << i;
        }
    }
    EXPECT_EQ(d[0].seq(), params.first_sequence);

    // First datagram: PAT and PMT; then the content from the start entry.
    ASSERT_EQ(d[0].packets(), 2u);
    EXPECT_EQ(std::memcmp(d[0].packet(0), info.pat.data(), 188), 0);
    EXPECT_EQ(std::memcmp(d[0].packet(1), info.pmt.data(), 188), 0);
    EXPECT_EQ(d[1].data, [&] {
        std::vector<uint8_t> expected(d[1].data.begin(), d[1].data.begin() + 12);
        const auto bytes = read_bytes(path, start.offset, 7 * 188);
        expected.insert(expected.end(), bytes.begin(), bytes.end());
        return expected;
    }());

    // Loop boundary: the datagram with the first 7 packets of the file.
    const std::vector<uint8_t> file_head = read_bytes(path, 0, 7 * 188);
    size_t loop = 0;
    for (size_t i = 1; i < d.size(); ++i) {
        if (d[i].packets() == 7 && std::memcmp(d[i].packet(0), file_head.data(), 7 * 188) == 0) {
            EXPECT_EQ(loop, 0u) << "file offset 0 sent twice";
            loop = i;
        }
    }
    ASSERT_GT(loop, 1u) << "no datagram with file offset 0";
    const double loop_at = std::chrono::duration<double>(d[loop].at - d[0].at).count();
    std::printf("TST-314 measured: loop boundary after %.3f s (expected 3 s), gap at the boundary %.1f ms\n", loop_at,
                std::chrono::duration<double, std::milli>(d[loop].at - d[loop - 1].at).count());
    EXPECT_GT(loop_at, 2.8);
    EXPECT_LT(loop_at, 3.2);
    EXPECT_LT(d[loop].at - d[loop - 1].at, milliseconds(100)) << "pause at the loop boundary";
    // The block before it ends with the last packet of the file and has the remaining 1 - 7 packets.
    const int64_t remaining = (info.size - start.offset) / 188;
    const size_t last_block = remaining % 7 == 0 ? 7 : static_cast<size_t>(remaining % 7);
    EXPECT_EQ(d[loop - 1].packets(), last_block);
    const auto file_tail = read_bytes(path, info.size - 188, 188);
    EXPECT_EQ(std::memcmp(d[loop - 1].packet(d[loop - 1].packets() - 1), file_tail.data(), 188), 0);
    for (size_t i = 1; i < d.size(); ++i) {
        if (i != loop - 1) {
            EXPECT_EQ(d[i].packets(), 7u) << i;
        }
    }

    // Average bitrate over the last 10 s within +-5 % of the multiplex rate.
    const auto window_start = d.back().at - seconds(10);
    double bytes = 0;
    for (const Datagram& x : d) {
        if (x.at > window_start) bytes += static_cast<double>(x.packets() * 188);
    }
    const double bitrate = bytes * 8 / 10.0;
    std::printf("TST-314 measured: bitrate %.0f bit/s (3500000 +/- 5 %%)\n", bitrate);
    EXPECT_NEAR(bitrate, kMuxRate, kMuxRate * 0.05) << bitrate;
}

// ---------------------------------------------------------------------------------------------
// TST-315 — SRV-RTP: skip forward when more than 1 s late (REQ-66, D-08, Section 4.7.5 step 4)
// ---------------------------------------------------------------------------------------------

TEST(Tst315, SenderSkipsForwardAfterAClockJumpOf2s) {
    const srv::ServiceCatalog catalog = test_catalog();
    const auto archive = archive_of(service(catalog, "svc-1"));
    ASSERT_TRUE(archive);
    const FileInfo info = scan_file(content_file("svc-1.ts"));
    test::TempDir dir;
    srv::Logger logger(dir.file("headend.log"));
    UdpReceiver rx;
    const int send_fd = udp_send_socket();

    // Advancing monotonic clock = real clock + a shift that jumps by 2 s once (e.g. a suspended process).
    std::atomic<int64_t> shift_ns{0};
    srv::MonoClock clock = [&shift_ns] { return steady_clock::now() + std::chrono::nanoseconds(shift_ns.load()); };
    srv::SleepFn sleep = [&shift_ns](steady_clock::time_point t) {
        std::this_thread::sleep_until(t - std::chrono::nanoseconds(shift_ns.load()));
    };
    srv::RtpSessionParams params;
    params.session_id = "hs-test315";
    params.client_host = "127.0.0.1";
    params.client_port = static_cast<uint16_t>(rx.port());
    params.ssrc = 315;
    const srv::Archive::Position start = archive->position(kCycle0 + 100);
    srv::RtpSender sender(archive, send_fd, params, start, clock, sleep, &logger);
    sender.start();
    std::thread jump([&shift_ns] {
        std::this_thread::sleep_for(milliseconds(1000));
        shift_ns = std::chrono::nanoseconds(seconds(2)).count();
    });
    const std::vector<Datagram> d = rx.receive_for(milliseconds(3000));
    jump.join();
    sender.stop();
    close(send_fd);
    ASSERT_GT(d.size(), 500u);

    // Content time of a datagram relative to the start, from its RTP timestamp (C - c0).
    auto content = [&d](size_t i) { return static_cast<int32_t>(d[i].ts() - d[0].ts()) / 90000.0; };
    size_t skip = 0;
    for (size_t i = 1; i < d.size(); ++i) {
        EXPECT_EQ(static_cast<uint16_t>(d[i - 1].seq() + 1), d[i].seq()) << i;
        const double step = content(i) - content(i - 1);
        if (step > 1.0) {
            EXPECT_EQ(skip, 0u) << "more than one skip";
            skip = i;
            // 2 s of content skipped (plus at most one block and one PCR interval), no block of them sent.
            std::printf("TST-315 measured: content skipped by %.4f s at block %zu (2 s within one PCR interval)\n",
                        step, i);
            EXPECT_GT(step, 2.0 - kMaxPcrInterval);
            EXPECT_LT(step, 2.0 + kMaxPcrInterval + 0.005);
        }
    }
    ASSERT_GT(skip, 0u) << "no skip";
    // The block after the skip starts at an index entry: a packet of the PCR PID whose PCR is the
    // content time of the block.
    const uint8_t* first = d[skip].packet(0);
    EXPECT_EQ(pid_of(first), 256);
    ASSERT_GE(pcr_of(first), 0);
    EXPECT_EQ(pcr_of(first) - info.first_pcr, start.c + static_cast<int32_t>(d[skip].ts() - d[0].ts()));

    // Before and after the skip every block is on the timeline c0 + (now - w0) within one PCR
    // interval (receive jitter of the test allowed: 10 ms); no burst, no shifted timeline.
    double worst = 0;
    for (size_t i = 1; i < d.size(); ++i) {
        const double elapsed = std::chrono::duration<double>(d[i].at - d[0].at).count() + (i >= skip ? 2.0 : 0.0);
        worst = std::max(worst, std::fabs(content(i) - elapsed));
        EXPECT_NEAR(content(i), elapsed, kMaxPcrInterval + 0.010) << i;
    }
    std::printf("TST-315 measured: largest deviation from the timeline %.4f s (limit %.4f s)\n", worst,
                kMaxPcrInterval + 0.010);
    const std::string log = test::read_text(dir.file("headend.log"));
    EXPECT_NE(log.find("WARN [SRV-RTP] late by "), std::string::npos) << log;
    EXPECT_NE(log.find(" s, skipping to content time "), std::string::npos) << log;
}

// ---------------------------------------------------------------------------------------------
// TST-316 — SRV-HEC: IF-EXT-04 examples, errors, one session per endpoint (REQ-65, D-06)
// ---------------------------------------------------------------------------------------------

TEST(Tst316, ControlApiExamplesOfSection33) {
    srv::HeadEndOptions options;
    options.wall_clock = fixed_wall_clock(kT);
    const auto he = start_head_end(options);
    ASSERT_TRUE(he);
    UdpReceiver rx;

    // POST /start of Section 3.3.1 (archive) -> 200 with the fields of the example.
    test::HttpReply r = he->post("start", start_body("svc-3", 1794999600, rx.port(), 3054235519u));
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body), json::parse(R"({"session_id": "hs-000001", "service_id": "svc-3",
        "start_time": 1794999600, "ssrc": 3054235519, "payload_type": 33})"));
    std::vector<Datagram> d = rx.receive_for(milliseconds(300));
    ASSERT_GT(d.size(), 10u);
    EXPECT_EQ(d[0].packets(), 2u);  // PAT and PMT first
    for (const Datagram& x : d) EXPECT_EQ(x.ssrc(), 3054235519u);

    // POST /keepalive, POST /stop of Sections 3.3.3 and 3.3.2.
    r = he->post("keepalive", json{{"session_id", "hs-000001"}});
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body), json::parse(R"({"expires_in_s": 15})"));
    r = he->post("stop", json{{"session_id", "hs-000001"}});
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body), json::parse(R"({"stopped": true})"));
    rx.drain();  // sent before the response
    EXPECT_TRUE(rx.receive_for(milliseconds(300)).empty()) << "datagram after the stop response";
    EXPECT_TRUE(he->head_end->session_ids().empty());

    // Stopped session: SESSION_NOT_FOUND for stop and keepalive.
    expect_error(he->post("stop", json{{"session_id", "hs-000001"}}), 404, "SESSION_NOT_FOUND");
    expect_error(he->post("keepalive", json{{"session_id", "hs-000001"}}), 404, "SESSION_NOT_FOUND");

    // "live": start_time is the server time.
    r = he->post("start", start_body("svc-1", "live", rx.port(), 1180331842u));
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body), json::parse(R"({"session_id": "hs-000002", "service_id": "svc-1",
        "start_time": 1795000000, "ssrc": 1180331842, "payload_type": 33})"));
}

TEST(Tst316, InvalidStartRequestsIn02AndIn03) {
    srv::HeadEndOptions options;
    options.wall_clock = fixed_wall_clock(kT);
    const auto he = start_head_end(options);
    ASSERT_TRUE(he);
    const int port = 5004;
    const json valid = start_body("svc-1", kT - 60, port, 7);

    // IN-02 -> 400 INVALID_REQUEST.
    json no_ssrc = valid;
    no_ssrc.erase("ssrc");
    const std::vector<std::pair<const char*, json>> in02 = {
        {"without ssrc", no_ssrc},
        {"ssrc -1", with(start_body("svc-1", kT - 60, port, 0), "ssrc", -1)},
        {"ssrc 4294967296", with(start_body("svc-1", kT - 60, port, 0), "ssrc", 4294967296LL)},
        {"ssrc abc", with(start_body("svc-1", kT - 60, port, 0), "ssrc", "abc")},
        {"start yesterday", start_body("svc-1", "yesterday", port, 7)},
        {"client_port 70000", start_body("svc-1", kT - 60, 70000, 7)},
        // Further malformed fields.
        {"start 1.5", start_body("svc-1", 1.5, port, 7)},
        {"client_host name", with(start_body("svc-1", kT - 60, port, 7), "client_host", "localhost")},
        {"client_port 0", start_body("svc-1", kT - 60, 0, 7)},
        {"service_id number", with(start_body("svc-1", kT - 60, port, 7), "service_id", 1)},
        {"body array", json::array({1, 2})},
    };
    for (const auto& [what, body] : in02) {
        SCOPED_TRACE(what);
        expect_error(he->post("start", body), 400, "INVALID_REQUEST");
    }
    // An invalid field is reported before an unknown service (validation order of Section 4.7.6).
    expect_error(he->post("start", start_body("svc-9", "yesterday", port, 7)), 400, "INVALID_REQUEST");

    // IN-03: T = now - 7201; T = now; svc-9.
    expect_error(he->post("start", start_body("svc-1", kT - 7201, port, 7)), 400, "OUT_OF_ARCHIVE");
    expect_error(he->post("start", start_body("svc-1", kT, port, 7)), 400, "FUTURE_TIME");
    expect_error(he->post("start", start_body("svc-9", kT - 60, port, 7)), 404, "SERVICE_NOT_FOUND");
    EXPECT_TRUE(he->head_end->session_ids().empty()) << "a rejected request created a session";

    // The oldest valid time.
    const test::HttpReply r = he->post("start", start_body("svc-1", kT - 7200, port, 7));
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(he->head_end->session_ids().size(), 1u);
}

TEST(Tst316, NewStartForTheSameEndpointReplacesTheSession) {
    srv::HeadEndOptions options;
    options.wall_clock = fixed_wall_clock(kT);
    const auto he = start_head_end(options);
    ASSERT_TRUE(he);
    UdpReceiver rx;

    // Two /start for the same endpoint with SSRC A, then B: after the second response no datagram of A.
    ASSERT_EQ(he->post("start", start_body("svc-1", "live", rx.port(), 0xAAAA)).status, 200);
    EXPECT_FALSE(rx.receive_for(milliseconds(200)).empty());
    ASSERT_EQ(he->post("start", start_body("svc-2", kT - 3600, rx.port(), 0xBBBB)).status, 200);
    // Queued before the response: A, stopped and joined before B was created, then the start of B.
    const std::vector<Datagram> before = rx.drain();
    const std::vector<Datagram> after = rx.receive_for(milliseconds(300));
    ASSERT_FALSE(after.empty());
    for (const Datagram& x : after) EXPECT_EQ(x.ssrc(), 0xBBBBu);
    std::vector<Datagram> all = before;
    all.insert(all.end(), after.begin(), after.end());
    const auto first_b = std::find_if(all.begin(), all.end(), [](const Datagram& x) { return x.ssrc() == 0xBBBBu; });
    ASSERT_NE(first_b, all.end());
    EXPECT_EQ(first_b->packets(), 2u);  // B starts with PAT and PMT
    for (auto it = first_b; it != all.end(); ++it) EXPECT_EQ(it->ssrc(), 0xBBBBu) << "datagram of A after B started";
    EXPECT_EQ(he->head_end->session_ids(), std::vector<std::string>{"hs-000002"});
    expect_error(he->post("keepalive", json{{"session_id", "hs-000001"}}), 404, "SESSION_NOT_FOUND");

    // A different endpoint gets its own session.
    UdpReceiver rx2;
    ASSERT_EQ(he->post("start", start_body("svc-1", "live", rx2.port(), 0xCCCC)).status, 200);
    EXPECT_EQ(he->head_end->session_ids().size(), 2u);
}

TEST(Tst316, TenConcurrentStartsForOneEndpointLeaveExactlyOneSession) {
    srv::HeadEndOptions options;
    options.wall_clock = fixed_wall_clock(kT);
    const auto he = start_head_end(options);
    ASSERT_TRUE(he);
    UdpReceiver rx;

    std::vector<std::thread> clients;
    std::vector<int> status(10, 0);
    for (int i = 0; i < 10; ++i) {
        clients.emplace_back([&, i] {
            status[static_cast<size_t>(i)] =
                he->post("start", start_body("svc-1", "live", rx.port(), static_cast<uint32_t>(1000 + i))).status;
        });
    }
    for (auto& c : clients) c.join();
    for (int s : status) EXPECT_EQ(s, 200);
    EXPECT_EQ(he->head_end->session_ids().size(), 1u);
    rx.drain();
    std::set<uint32_t> ssrcs;
    for (const Datagram& x : rx.receive_for(milliseconds(300))) ssrcs.insert(x.ssrc());
    EXPECT_EQ(ssrcs.size(), 1u);
}

// ---------------------------------------------------------------------------------------------
// TST-317 — watchdog: expiry 15 - 16 s after the last start / keepalive (REQ-68, ER-17)
// ---------------------------------------------------------------------------------------------

TEST(Tst317, SessionWithoutKeepaliveExpiresBetween15And16s) {
    test::TempDir dir;
    srv::Logger logger(dir.file("headend.log"));
    // Advancing session clock driven by the test; the watchdog pass every 1 s is called by the test
    // (phase 0.5 s); the senders run on the real clock.
    const auto epoch = steady_clock::now();
    std::atomic<int64_t> now_ms{0};
    srv::HeadEndOptions options;
    options.wall_clock = fixed_wall_clock(kT);
    options.session_clock = [epoch, &now_ms] { return epoch + milliseconds(now_ms.load()); };
    options.start_watchdog = false;
    const auto he = start_head_end(options, &logger);
    ASSERT_TRUE(he);
    UdpReceiver rx_a, rx_b;

    ASSERT_EQ(he->post("start", start_body("svc-1", "live", rx_a.port(), 1)).status, 200);  // hs-000001
    ASSERT_EQ(he->post("start", start_body("svc-2", "live", rx_b.port(), 2)).status, 200);  // hs-000002
    int64_t expired_at_ms = -1;
    for (int64_t t = 250; t <= 60000; t += 250) {
        now_ms = t;
        if (t % 5000 == 0) {  // keepalive of hs-000001 every 5 s
            EXPECT_EQ(he->post("keepalive", json{{"session_id", "hs-000001"}}).status, 200) << t;
        }
        if (t % 1000 == 500) he->head_end->watchdog_tick();
        const auto ids = he->head_end->session_ids();
        if (expired_at_ms < 0 && std::find(ids.begin(), ids.end(), "hs-000002") == ids.end()) expired_at_ms = t;
    }
    std::printf("TST-317 measured: session without keepalive stopped after %lld ms (limit 15000 - 16000 ms)\n",
                static_cast<long long>(expired_at_ms));
    EXPECT_GE(expired_at_ms, 15000);
    EXPECT_LE(expired_at_ms, 16000);
    EXPECT_EQ(he->head_end->session_ids(), std::vector<std::string>{"hs-000001"});
    expect_error(he->post("keepalive", json{{"session_id", "hs-000002"}}), 404, "SESSION_NOT_FOUND");
    rx_b.drain();
    EXPECT_TRUE(rx_b.receive_for(milliseconds(200)).empty()) << "expired session still sends";
    EXPECT_FALSE(rx_a.receive_for(milliseconds(200)).empty()) << "kept-alive session does not send";

    const std::string log = test::read_text(dir.file("headend.log"));
    std::printf("TST-317 log (headend.log of the test):\n%s", log.c_str());  // LOG evidence (5.1.6)
    EXPECT_NE(log.find("WARN [SRV-HEC] session expired session_id=hs-000002"), std::string::npos) << log;
    EXPECT_EQ(log.find("session expired session_id=hs-000001"), std::string::npos) << log;
}

TEST(Tst317, WatchdogThreadStopsExpiredSessions) {
    // The real watchdog thread and clocks, with the expiry shortened to 1 s and a 100 ms period.
    srv::HeadEndOptions options;
    options.session_expiry = seconds(1);
    options.watchdog_period = milliseconds(100);
    const auto he = start_head_end(options);
    ASSERT_TRUE(he);
    UdpReceiver rx;
    const auto start = steady_clock::now();
    ASSERT_EQ(he->post("start", start_body("svc-1", "live", rx.port(), 9)).status, 200);
    while (!he->head_end->session_ids().empty() && steady_clock::now() - start < seconds(3)) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    const double expired = std::chrono::duration<double>(steady_clock::now() - start).count();
    EXPECT_TRUE(he->head_end->session_ids().empty());
    EXPECT_GE(expired, 1.0);
    EXPECT_LT(expired, 1.3);
}

TEST(Tst317, LoggerKeepsLinesWholeFromSeveralThreads) {
    // The LOG evidence of TST-317 (and the log keywords of every head-end test) relies on the logger
    // of srvcommon writing whole lines while the HTTP thread, the watchdog and the sender threads log
    // at the same time (NFR-13).
    test::TempDir dir;
    const std::string path = dir.file("a/b/headend.log");  // the directory does not exist yet
    {
        srv::Logger logger(path);
        std::vector<std::thread> threads;
        for (int k = 0; k < 4; ++k) {
            threads.emplace_back([&logger, k] {
                for (int i = 0; i < 1000; ++i) {
                    logger.info("SRV-T", "thread " + std::to_string(k) + " line " + std::to_string(i) + " with\nbreak");
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    const std::regex line_format(
        R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3} INFO \[SRV-T\] thread (\d) line (\d+) with\?break$)");
    std::istringstream lines(test::read_text(path));
    std::string line;
    std::set<std::string> seen;
    while (std::getline(lines, line)) {
        std::smatch m;
        ASSERT_TRUE(std::regex_match(line, m, line_format)) << line;
        EXPECT_TRUE(seen.insert(m[1].str() + "/" + m[2].str()).second) << "duplicate " << line;
    }
    EXPECT_EQ(seen.size(), 4000u);

    // Time stamp of NFR-13: local time with milliseconds (here with TZ=UTC: the time of Section 5.2.4).
    const char* old_tz = std::getenv("TZ");
    const std::string saved = old_tz ? old_tz : "";
    setenv("TZ", "UTC", 1);
    tzset();
    EXPECT_EQ(srv::format_log_time(srv::from_unix_seconds(kT) + milliseconds(317)), "2026-11-18T11:06:40.317");
    if (old_tz) {
        setenv("TZ", saved.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();

    // A log file that cannot be created: error naming the file.
    try {
        srv::Logger bad("/proc/no-such-dir/headend.log");
        ADD_FAILURE() << "no error";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("/proc/no-such-dir/headend.log"), std::string::npos) << e.what();
    }
}

// ---------------------------------------------------------------------------------------------
// TST-318 — invalid requests to the head-end (REQ-58, ER-18); OTT MW server part in mw_test.cpp
// ---------------------------------------------------------------------------------------------

TEST(Tst318, HeadEndAnswersIn01WithErrorObjectsAndKeepsRunning) {
    srv::HeadEndOptions options;
    options.wall_clock = fixed_wall_clock(kT);
    const auto he = start_head_end(options);
    ASSERT_TRUE(he);
    const int port = he->server->port();
    std::mt19937 rng(318);
    std::string random_bytes(4096, '\0');
    for (auto& c : random_bytes) c = static_cast<char>(rng() & 0xFF);
    auto expect_400_or_404 = [](const test::HttpReply& reply, const char* what) {
        SCOPED_TRACE(what);
        ASSERT_TRUE(reply.answered);
        EXPECT_TRUE(reply.status == 400 || reply.status == 404) << reply.status;
        expect_error(reply, reply.status, "");
    };

    // IN-01: truncated JSON body; body of 4 096 random bytes; unknown path; method DELETE.
    expect_400_or_404(test::http_exchange(port, post_request("/api/v1/stream/start", "{\"service_id\": ")),
                      "truncated JSON");
    expect_400_or_404(test::http_exchange(port, post_request("/api/v1/stream/start", random_bytes)), "random body");
    expect_400_or_404(test::http_get(port, "/api/v1/xyz"), "unknown path");
    expect_400_or_404(test::http_exchange(port, "DELETE /api/v1/stream/start HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"),
                      "DELETE");
    // Further: the same on /stop and /keepalive, GET of a known path, not HTTP at all.
    expect_400_or_404(test::http_exchange(port, post_request("/api/v1/stream/stop", random_bytes)), "random stop");
    expect_400_or_404(test::http_exchange(port, post_request("/api/v1/stream/keepalive", "{")), "truncated keepalive");
    expect_400_or_404(test::http_get(port, "/api/v1/stream/start"), "GET start");
    test::http_exchange(port, random_bytes);
    EXPECT_TRUE(he->head_end->session_ids().empty());

    // The head-end still answers a valid request.
    UdpReceiver rx;
    const test::HttpReply r = he->post("start", start_body("svc-1", "live", rx.port(), 42));
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(he->post("stop", json{{"session_id", json::parse(r.body)["session_id"]}}).status, 200);
}

// ---------------------------------------------------------------------------------------------
// TST-320 — missing or invalid configuration (ER-37), head-end part; OTT MW part in mw_test.cpp
// ---------------------------------------------------------------------------------------------

TEST(Tst320, HeadEndExitsWithCode1NamingTheFile) {
    test::TempDir dir;
    const int port = test::free_tcp_port();
    struct Case {
        const char* what;
        std::string config;
        std::string expected_name;
    };
    std::vector<Case> cases;
    cases.push_back({"config missing", dir.file("missing-config.json"), dir.file("missing-config.json")});
    test::write_text(dir.file("bad-config.json"), R"({"headend_port": )");
    cases.push_back({"config invalid JSON", dir.file("bad-config.json"), dir.file("bad-config.json")});
    test::write_text(dir.file("cfg-missing-services.json"), head_end_config(dir, dir.file("missing-services.json"), port));
    cases.push_back({"services missing", dir.file("cfg-missing-services.json"), dir.file("missing-services.json")});
    test::write_text(dir.file("bad-services.json"), R"({"archive_window_s": 7200, "services": [ {)");
    test::write_text(dir.file("cfg-bad-services.json"), head_end_config(dir, dir.file("bad-services.json"), port));
    cases.push_back({"services invalid JSON", dir.file("cfg-bad-services.json"), dir.file("bad-services.json")});
    json no_name = json::parse(test::read_text(kRepo + "/content/services.json"));
    no_name["services"][0].erase("name");
    test::write_text(dir.file("noname-services.json"), no_name.dump());
    test::write_text(dir.file("cfg-noname.json"), head_end_config(dir, dir.file("noname-services.json"), port));
    cases.push_back({"services invalid entry", dir.file("cfg-noname.json"), dir.file("noname-services.json")});

    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        const auto [code, output] = run_head_end(dir, c.config);
        EXPECT_EQ(code, 1) << output;
        EXPECT_NE(output.find(c.expected_name), std::string::npos) << output;
    }
}

TEST(Tst320, HeadEndStartsWithValidConfigStreamsLogsAndStopsOnSigterm) {
    // Control case: valid files -> the head-end loads, streams, logs in the NFR-13 format into a log
    // directory it creates, and exits with 0 on SIGTERM.
    test::TempDir dir;
    const int port = test::free_tcp_port();
    json services = json::parse(test::read_text(kRepo + "/content/services.json"));
    services["services"] = json::array({services["services"][0]});  // svc-1 only (clear content)
    test::write_text(dir.file("services.json"), services.dump());
    json c = json::parse(head_end_config(dir, dir.file("services.json"), port));
    c["log_dir"] = dir.file("logs/nested");
    test::write_text(dir.file("config.json"), c.dump());

    test::Process p({kHeadEndBin, "--config", dir.file("config.json")}, dir.file("output.txt"));
    ASSERT_TRUE(p.started());
    UdpReceiver rx;
    test::HttpReply reply;
    for (int i = 0; i < 200 && !reply.answered; ++i) {
        std::this_thread::sleep_for(milliseconds(50));
        reply = test::http_exchange(port, post_request("/api/v1/stream/start",
                                                       start_body("svc-1", "live", rx.port(), 77).dump()));
    }
    ASSERT_TRUE(reply.answered) << test::read_text(dir.file("output.txt"));
    EXPECT_EQ(reply.status, 200) << reply.body;
    const auto d = rx.receive_for(milliseconds(500));
    EXPECT_GT(d.size(), 100u);
    p.signal(SIGTERM);
    EXPECT_EQ(p.wait(seconds(5)), 0);

    const std::string log = test::read_text(dir.file("logs/nested/headend.log"));
    const std::regex line_format(
        R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3} (DEBUG|INFO|WARN|ERROR) \[SRV-(HEC|ARC|RTP)\] .+$)");
    std::istringstream lines(log);
    std::string line;
    while (std::getline(lines, line)) EXPECT_TRUE(std::regex_match(line, line_format)) << line;
    EXPECT_NE(log.find("INFO [SRV-ARC] loaded service=svc-1"), std::string::npos) << log;
    EXPECT_NE(log.find("INFO [SRV-HEC] listening on 127.0.0.1:" + std::to_string(port)), std::string::npos) << log;
    EXPECT_NE(log.find("INFO [SRV-HEC] start session_id=hs-000001 service=svc-1 start=live client=127.0.0.1:" +
                       std::to_string(rx.port()) + " ssrc=77"),
              std::string::npos)
        << log;
    EXPECT_NE(log.find("INFO [SRV-HEC] stop session_id=hs-000001 reason=shutdown"), std::string::npos) << log;
    EXPECT_NE(log.find("INFO [SRV-HEC] stopped by SIGTERM"), std::string::npos) << log;
}
