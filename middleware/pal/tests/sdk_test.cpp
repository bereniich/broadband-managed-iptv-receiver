// Unit tests of PAL-SDK (Section 5.3.3): TST-221 without the key part (step 4 of TST-221 follows with
// HAL-DSC). The DTV SDK is taken from a Platform of create_platform with a test configuration, as the
// precondition of TST-221 says. Data enters through the real tuner: RTP datagrams to 127.0.0.1, as the
// head-end sends them.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "platform_fixture.hpp"
#include "ts_helper.hpp"

namespace {

using pal::FilterConfig;
using pal::FilterType;
using pal::IDemux;
using pal::LogLevel;
using pal::TunerStatus;
using std::chrono::milliseconds;
using testhelp::Bytes;
using testhelp::Packet;
using testutil::wait_until;

constexpr std::uint32_t kSsrc = 1180331842;

struct Delivery {
    IDemux::FilterId filter;
    Bytes data;
    bool scrambled;
};

// Collects FilterData events (copied, Section 3.9.1 rule 3).
struct Sink {
    std::mutex mutex;
    std::vector<Delivery> items;
    IDemux::DataCallback callback() {
        return [this](const IDemux::FilterData& d) {
            std::lock_guard<std::mutex> lock(mutex);
            items.push_back({d.filter, Bytes(d.data, d.data + d.len), d.scrambled});
        };
    }
    std::size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return items.size();
    }
    std::vector<Delivery> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::move(items);
    }
};

// The DTV SDK as the AL gets it: Platform::tuner() and Platform::demux() of a started platform.
class Sdk {
public:
    Sdk() : p(t.create()), demux(p.demux()), tuner(p.tuner()) {
        if (!p.start()) throw std::runtime_error("Platform::start failed");
    }
    std::uint16_t port() { return t.linux_platform().tuner_port(); }
    // Number of log lines "<LEVEL> [<module>] …<text>…".
    int log_count(const std::string& level, const std::string& module, const std::string& text) const {
        std::istringstream in(t.log_text());
        int n = 0;
        for (std::string line; std::getline(in, line);) {
            if (line.find(" " + level + " [" + module + "] ") != std::string::npos && line.find(text) != std::string::npos) ++n;
        }
        return n;
    }

    testutil::TestPlatform t;
    pal::Platform& p;
    pal::IDemux& demux;
    pal::ITuner& tuner;
};

// Sends TS packets as RTP datagrams of up to 7 packets with consecutive sequence numbers.
class Sender {
public:
    explicit Sender(std::uint16_t port) : port_(port) {}
    void send(const std::vector<Packet>& packets, std::uint32_t ssrc = kSsrc) {
        for (std::size_t i = 0; i < packets.size(); i += 7) {
            std::vector<Packet> block(packets.begin() + static_cast<std::ptrdiff_t>(i),
                                      packets.begin() + static_cast<std::ptrdiff_t>(std::min(i + 7, packets.size())));
            testhelp::RtpHeader h;
            h.sequence = sequence_++;
            h.ssrc = ssrc;
            udp_.send(port_, testhelp::rtp_datagram(h, block));
        }
    }
    void skip_sequence() { ++sequence_; }
    void send_raw(const Bytes& datagram) { udp_.send(port_, datagram); }

private:
    std::uint16_t port_;
    std::uint16_t sequence_ = 1000;
    testhelp::UdpSender udp_;
};

std::vector<Packet> video_packets(int count, std::uint8_t& cc, bool scrambled = false) {
    std::vector<Packet> out;
    for (int i = 0; i < count; ++i) {
        Packet p = testhelp::packet(256, cc, false, Bytes(184, static_cast<std::uint8_t>(i)));
        if (scrambled) p[3] = static_cast<std::uint8_t>(p[3] | 0x80);  // transport_scrambling_control '10'
        out.push_back(p);
        cc = static_cast<std::uint8_t>((cc + 1) & 0x0F);
    }
    return out;
}

// Step 1: an invalid configuration gives 0; valid filters get distinct non-zero handles.
TEST(Tst221, OpenFilterReturnsZeroForPid8192AndOtherInvalidConfigurations) {
    Sdk sdk;
    Sink sink;
    EXPECT_EQ(sdk.demux.open_filter({FilterType::Ts, 8192}, sink.callback()), 0u);
    EXPECT_EQ(sdk.demux.open_filter({FilterType::Section, 8192, 0}, sink.callback()), 0u);
    EXPECT_EQ(sdk.demux.open_filter({FilterType::Section, 0, 256}, sink.callback()), 0u);
    EXPECT_EQ(sdk.demux.open_filter({FilterType::Section, 0, -2}, sink.callback()), 0u);
    EXPECT_EQ(sdk.demux.open_filter({FilterType::Ts, 256}, nullptr), 0u);
    EXPECT_EQ(sdk.demux.open_filter({static_cast<FilterType>(7), 256}, sink.callback()), 0u);
    const auto a = sdk.demux.open_filter({FilterType::Ts, 8191}, sink.callback());
    const auto b = sdk.demux.open_filter({FilterType::Section, 0, 0}, sink.callback());
    const auto c = sdk.demux.open_filter({FilterType::Ts, 256, 99}, sink.callback());  // table_id ignored
    EXPECT_NE(a, 0u);
    EXPECT_NE(b, 0u);
    EXPECT_NE(c, 0u);
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_FALSE(sdk.demux.start_filter(12345));
}

// Steps 2 and 3: no data before start_filter, data with the filter handle after it, none after
// stop_filter or close_filter. Section filters deliver complete sections, TS filters whole packets
// with the scrambled flag of transport_scrambling_control.
TEST(Tst221, NoDataBeforeStartFilterAndNoneAfterStopOrClose) {
    Sdk sdk;
    ASSERT_TRUE(sdk.tuner.tune({0, kSsrc}));
    Sender sender(sdk.port());
    Sink ts_sink, section_sink;
    const auto ts = sdk.demux.open_filter({FilterType::Ts, 256}, ts_sink.callback());
    const auto pat = sdk.demux.open_filter({FilterType::Section, 0, 0x00}, section_sink.callback());
    ASSERT_NE(ts, 0u);
    ASSERT_NE(pat, 0u);

    std::uint8_t cc256 = 0, cc0 = 0;
    const Bytes pat_section = testhelp::section(0x00, 16, 3);
    auto feed = [&](int video_count, bool scrambled = false) {
        std::vector<Packet> packets = testhelp::packetize(0, cc0, pat_section, {});
        const auto video = video_packets(video_count, cc256, scrambled);
        packets.insert(packets.end(), video.begin(), video.end());
        const auto before = sdk.demux.stats().ts_packets;
        sender.send(packets);
        ASSERT_TRUE(wait_until([&] { return sdk.demux.stats().ts_packets == before + packets.size(); }));
    };

    feed(13);  // 2: not started yet
    EXPECT_EQ(ts_sink.size(), 0u);
    EXPECT_EQ(section_sink.size(), 0u);

    ASSERT_TRUE(sdk.demux.start_filter(ts));
    ASSERT_TRUE(sdk.demux.start_filter(pat));
    feed(13);
    feed(1, true);
    auto packets = ts_sink.take();
    ASSERT_EQ(packets.size(), 14u);
    for (std::size_t i = 0; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].filter, ts);
        ASSERT_EQ(packets[i].data.size(), 188u);
        EXPECT_EQ(packets[i].data[0], 0x47);
        EXPECT_EQ(((packets[i].data[1] & 0x1F) << 8) | packets[i].data[2], 256);
        EXPECT_EQ(packets[i].scrambled, i == 13);
    }
    auto sections = section_sink.take();
    ASSERT_EQ(sections.size(), 2u);
    for (const auto& s : sections) {
        EXPECT_EQ(s.filter, pat);
        EXPECT_EQ(s.data, pat_section);
        EXPECT_FALSE(s.scrambled);
    }

    sdk.demux.stop_filter(ts);  // 3
    sdk.demux.stop_filter(pat);
    feed(7);
    EXPECT_EQ(ts_sink.size(), 0u);
    EXPECT_EQ(section_sink.size(), 0u);
    ASSERT_TRUE(sdk.demux.start_filter(ts));
    feed(7);
    EXPECT_EQ(ts_sink.take().size(), 7u);
    sdk.demux.close_filter(ts);
    sdk.demux.close_filter(pat);
    feed(7);
    EXPECT_EQ(ts_sink.size(), 0u);
    EXPECT_EQ(section_sink.size(), 0u);
    EXPECT_FALSE(sdk.demux.start_filter(ts));
    sdk.tuner.untune();
}

// Step 4 without the key: the statistics of ITuner and IDemux equal the counters of the HAL for a
// known input, and tune() starts a new set of counters.
TEST(Tst221, StatisticsEqualTheHalCountersAndTuneClearsThem) {
    Sdk sdk;
    ASSERT_TRUE(sdk.tuner.tune({0, kSsrc}));
    Sender sender(sdk.port());

    std::uint8_t cc = 0;
    std::vector<Packet> packets = video_packets(14, cc);  // 2 datagrams
    packets[3][0] = 0x46;  // 1 sync error: dropped, so packet 4 follows packet 2 (1 cc error)
    packets[9][3] = static_cast<std::uint8_t>((packets[9][3] & 0xF0) | ((packets[9][3] + 2) & 0x0F));
    // packet 9 jumps from 8 to 11, packet 10 goes back from 11 to 10: 2 more cc errors
    sender.send(packets);
    sender.skip_sequence();  // 1 lost datagram
    sender.send(video_packets(7, cc));
    Bytes invalid = testhelp::rtp_datagram({0x80, 96, 1, 0, kSsrc}, video_packets(1, cc));  // PT 96
    sender.send_raw(invalid);
    ASSERT_TRUE(wait_until([&] {
        const auto t = sdk.tuner.stats();
        return t.rtp_packets == 3 && t.rtp_invalid == 1;
    }));
    const auto t = sdk.tuner.stats();
    EXPECT_EQ(t.rtp_packets, 3u);
    EXPECT_EQ(t.rtp_lost, 1u);
    EXPECT_EQ(t.rtp_invalid, 1u);
    const auto d = sdk.demux.stats();
    EXPECT_EQ(d.ts_packets, 21u);
    EXPECT_EQ(d.sync_errors, 1u);
    EXPECT_EQ(d.cc_errors, 3u);

    ASSERT_TRUE(sdk.tuner.tune({0, kSsrc}));
    const auto t2 = sdk.tuner.stats();
    const auto d2 = sdk.demux.stats();
    EXPECT_EQ(t2.rtp_packets + t2.rtp_lost + t2.rtp_invalid, 0u);
    EXPECT_EQ(d2.ts_packets + d2.sync_errors + d2.cc_errors, 0u);
}

// ITuner contract of Section 3.9.2: Unlocked after tune() without a callback, Locked at the first
// accepted packet, Unlocked after 3 s without packets, on the receive thread with SIGINT and SIGTERM
// blocked; tune() returns false for a port in use, and the HAL error reaches the PAL log.
TEST(Tst221, TunerStatusThroughTheSdkAndPortInUse) {
    Sdk sdk;
    std::mutex mutex;
    std::vector<TunerStatus> statuses;
    std::atomic<bool> blocked{true};
    sdk.tuner.set_status_handler([&](TunerStatus s) {
        if (!testutil::stop_signals_blocked()) blocked = false;
        std::lock_guard<std::mutex> lock(mutex);
        statuses.push_back(s);
    });
    ASSERT_TRUE(sdk.tuner.tune({0, kSsrc}));
    EXPECT_EQ(sdk.log_count("INFO", "HAL-TUN", "SO_RCVBUF requested 1048576"), 1);
    Sender sender(sdk.port());
    std::uint8_t cc = 0;
    sender.send(video_packets(7, cc));
    auto count = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        return statuses.size();
    };
    ASSERT_TRUE(wait_until([&] { return count() == 2; }, milliseconds(4000)));
    {
        std::lock_guard<std::mutex> lock(mutex);
        EXPECT_EQ(statuses, (std::vector<TunerStatus>{TunerStatus::Locked, TunerStatus::Unlocked}));
    }
    EXPECT_TRUE(blocked.load());

    const int busy = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(busy, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    getsockname(busy, reinterpret_cast<sockaddr*>(&addr), &len);
    const std::uint16_t port = ntohs(addr.sin_port);
    EXPECT_FALSE(sdk.tuner.tune({port, kSsrc}));
    EXPECT_EQ(sdk.log_count("ERROR", "HAL-TUN", "cannot open UDP port 127.0.0.1:" + std::to_string(port)), 1);
    close(busy);
}

// Rules 2 and 5 of Section 3.9.1: a call into the SDK from its own callbacks cannot complete (the
// receive thread cannot join itself, the filter table is locked during delivery); it is refused and
// logged instead of hanging or throwing, and reception continues.
TEST(Tst221, CallsFromSdkCallbacksAreRefusedWithoutDeadlock) {
    Sdk sdk;
    std::atomic<int> refused_calls{0};
    IDemux::FilterId id = 0;
    std::atomic<bool> id_set{false};
    Sink sink;
    auto record = sink.callback();
    id = sdk.demux.open_filter({FilterType::Ts, 256}, [&](const IDemux::FilterData& d) {
        while (!id_set) std::this_thread::yield();
        if (sdk.demux.open_filter({FilterType::Ts, 257}, record) == 0) ++refused_calls;
        if (!sdk.demux.start_filter(id)) ++refused_calls;
        sdk.demux.close_filter(id);  // refused: the filter stays
        record(d);
    });
    id_set = true;
    sdk.tuner.set_status_handler([&](TunerStatus) {
        sdk.tuner.untune();                               // refused
        if (!sdk.tuner.tune({0, kSsrc})) ++refused_calls;  // refused
    });
    ASSERT_TRUE(sdk.demux.start_filter(id));
    ASSERT_TRUE(sdk.tuner.tune({0, kSsrc}));
    Sender sender(sdk.port());
    std::uint8_t cc = 0;
    sender.send(video_packets(7, cc));
    ASSERT_TRUE(wait_until([&] { return sink.size() == 7; }));
    sender.send(video_packets(7, cc));
    ASSERT_TRUE(wait_until([&] { return sink.size() == 14; }));  // still open, still tuned
    EXPECT_EQ(refused_calls.load(), 2 * 14 + 1);
    EXPECT_EQ(sdk.log_count("ERROR", "HAL-DMX", "called from a filter callback: refused"), 3 * 14);
    EXPECT_EQ(sdk.log_count("ERROR", "HAL-TUN", "called from a tuner callback: refused"), 2);
    sdk.tuner.untune();
}

}  // namespace

namespace {

// Step 4 of TST-221 (key part): IDemux::set_key reaches the descrambler (decrypted data, bits '00',
// scrambled = true), clear_keys() removes it, and the key never appears in the log (REQ-55).
TEST(Tst221, SetKeyReachesTheDescramblerAndClearKeysRemovesIt) {
    const char* k3_hex = "3A7F1C9E5B2D8046A1E3C5079B4D6F28";  // K3, Section 5.2.2
    const auto k3 = testhelp::key_from_hex(k3_hex);
    Sdk sdk;
    ASSERT_TRUE(sdk.tuner.tune({0, kSsrc}));
    Sender sender(sdk.port());
    Sink sink, audio_sink;
    const auto ts = sdk.demux.open_filter({FilterType::Ts, 256}, sink.callback());
    const auto audio = sdk.demux.open_filter({FilterType::Ts, 257}, audio_sink.callback());
    ASSERT_TRUE(sdk.demux.start_filter(ts));
    ASSERT_TRUE(sdk.demux.start_filter(audio));

    std::uint8_t cc = 0;
    std::vector<Packet> clear = video_packets(7, cc);
    std::vector<Packet> encrypted = clear;
    for (Packet& p : encrypted) ASSERT_TRUE(testhelp::d04_encrypt(p, k3));

    const std::uint16_t pids[] = {256, 257};
    sdk.demux.set_key(pids, 2, k3.data());
    sender.send(encrypted);
    ASSERT_TRUE(wait_until([&] { return sink.size() == 7; }));
    auto got = sink.take();
    for (std::size_t i = 0; i < 7; ++i) {
        EXPECT_EQ(got[i].data, Bytes(clear[i].begin(), clear[i].end())) << i;  // decrypted, bits '00'
        EXPECT_TRUE(got[i].scrambled) << i;
    }
    // The second PID of the same set_key (audio).
    const Packet audio_clear = testhelp::packet(257, 0, false, Bytes(184, 0x21));
    Packet audio_encrypted = audio_clear;
    ASSERT_TRUE(testhelp::d04_encrypt(audio_encrypted, k3));
    sender.send({audio_encrypted});
    ASSERT_TRUE(wait_until([&] { return audio_sink.size() == 1; }));
    EXPECT_EQ(audio_sink.take()[0].data, Bytes(audio_clear.begin(), audio_clear.end()));

    sdk.demux.clear_keys();
    std::vector<Packet> more = video_packets(7, cc);
    for (Packet& p : more) ASSERT_TRUE(testhelp::d04_encrypt(p, k3));
    sender.send(more);
    ASSERT_TRUE(wait_until([&] { return sink.size() == 7; }));
    got = sink.take();
    for (std::size_t i = 0; i < 7; ++i) {
        EXPECT_EQ(got[i].data, Bytes(more[i].begin(), more[i].end())) << i;  // unchanged, bits '10'
        EXPECT_TRUE(got[i].scrambled) << i;
    }

    sdk.demux.set_key(pids, 2, nullptr);  // invalid: refused and logged
    EXPECT_EQ(sdk.log_count("INFO", "PAL-SDK", "descrambler key set for PIDs 256,257"), 1);
    EXPECT_EQ(sdk.log_count("ERROR", "PAL-SDK", "set_key refused for PIDs 256,257"), 1);
    const std::string log = sdk.t.log_text();
    for (const std::string& secret : {std::string(k3_hex), std::string("3a7f1c9e5b2d8046a1e3c5079b4d6f28"),
                                      std::string("3A7F1"), std::string(reinterpret_cast<const char*>(k3.data()), 16)}) {
        EXPECT_EQ(log.find(secret), std::string::npos) << "key material in the log";
    }
    sdk.tuner.untune();
}

}  // namespace
