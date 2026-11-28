// Unit tests of HAL-TUN (Section 5.3.3): TST-201 – TST-206.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ip_tuner.hpp"
#include "ts_helper.hpp"

namespace {

using hal::IpTuner;
using std::chrono::milliseconds;
using std::chrono::steady_clock;
using testhelp::Bytes;
using testhelp::Packet;

constexpr std::uint32_t kSsrc = 1180331842;

// Events of the tuner callbacks in their order: "L" (Locked), "U" (Unlocked), packets.
struct Observer {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> events;
    std::vector<Bytes> packets;
    std::vector<std::pair<bool, steady_clock::time_point>> lock_changes;
    std::atomic<bool> untuned{false};
    std::atomic<int> callbacks_after_untune{0};
    std::atomic<int> callback_delay_ms{0};  // a slow callback widens the window that untune() must wait for

    IpTuner::PacketsCallback on_packets() {
        return [this](std::uint8_t* ts, std::size_t n) {
            if (untuned) ++callbacks_after_untune;
            if (callback_delay_ms > 0) {
                std::this_thread::sleep_for(milliseconds(callback_delay_ms.load()));
                if (untuned) ++callbacks_after_untune;  // still running after untune() returned
            }
            std::lock_guard<std::mutex> lock(mutex);
            for (std::size_t i = 0; i < n; ++i) packets.emplace_back(ts + 188 * i, ts + 188 * (i + 1));
            events.push_back("P" + std::to_string(n));
            cv.notify_all();
        };
    }
    IpTuner::LockCallback on_lock() {
        return [this](bool locked) {
            if (untuned) ++callbacks_after_untune;
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(locked ? "L" : "U");
            lock_changes.emplace_back(locked, steady_clock::now());
            cv.notify_all();
        };
    }
    bool wait_for(const std::function<bool()>& condition, milliseconds timeout = milliseconds(3000)) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, condition);
    }
    std::size_t packet_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return packets.size();
    }
};

// Seven TS packets whose payload bytes carry (datagram number, packet number).
std::vector<Packet> block(int datagram, std::uint8_t marker = 0) {
    std::vector<Packet> out;
    for (int k = 0; k < 7; ++k) {
        Bytes payload(184, marker);
        payload[0] = static_cast<std::uint8_t>(datagram >> 8);
        payload[1] = static_cast<std::uint8_t>(datagram);
        payload[2] = static_cast<std::uint8_t>(k);
        out.push_back(testhelp::packet(256, static_cast<std::uint8_t>(datagram * 7 + k), false, payload));
    }
    return out;
}

Bytes valid_datagram(std::uint16_t sequence, std::uint32_t ssrc, int datagram, std::uint8_t marker = 0) {
    testhelp::RtpHeader h;
    h.sequence = sequence;
    h.timestamp = static_cast<std::uint32_t>(datagram) * 270;
    h.ssrc = ssrc;
    return testhelp::rtp_datagram(h, block(datagram, marker));
}

// Waits until the tuner has processed `n` datagrams in total (accepted + invalid).
bool wait_processed(const IpTuner& tuner, std::uint64_t n, milliseconds timeout = milliseconds(3000)) {
    const auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
        const IpTuner::Stats s = tuner.stats();
        if (s.rtp_packets + s.rtp_invalid >= n) return true;
        std::this_thread::sleep_for(milliseconds(2));
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// TST-201 — valid datagrams: Locked once, 700 TS packets in order (REQ-13, B2)
// ---------------------------------------------------------------------------------------------

TEST(Tst201, HundredValidDatagramsLockOnceAndDeliver700PacketsInOrder) {
    Observer o;
    IpTuner tuner(o.on_packets(), o.on_lock());
    ASSERT_TRUE(tuner.tune(0, kSsrc));  // ephemeral port
    const std::uint16_t port = tuner.bound_port();
    ASSERT_NE(port, 0);
    testhelp::UdpSender sender;
    for (int i = 0; i < 100; ++i) {
        sender.send(port, valid_datagram(static_cast<std::uint16_t>(1000 + i), kSsrc, i));
        if (i % 20 == 19) {
            ASSERT_TRUE(wait_processed(tuner, static_cast<std::uint64_t>(i + 1)));
        }
    }
    ASSERT_TRUE(o.wait_for([&o] { return o.packets.size() >= 700; }));
    tuner.untune();

    EXPECT_EQ(tuner.stats().rtp_packets, 100u);
    EXPECT_EQ(tuner.stats().rtp_invalid, 0u);
    EXPECT_EQ(tuner.stats().rtp_lost, 0u);
    ASSERT_EQ(o.packets.size(), 700u);
    for (int i = 0; i < 100; ++i) {
        const std::vector<Packet> expected = block(i);
        for (int k = 0; k < 7; ++k) {
            EXPECT_EQ(o.packets[static_cast<std::size_t>(i * 7 + k)], Bytes(expected[k].begin(), expected[k].end()))
                << "datagram " << i << " packet " << k;
        }
    }
    ASSERT_GE(o.events.size(), 2u);
    EXPECT_EQ(o.events[0], "L");  // Locked at the first datagram, before its packets
    EXPECT_EQ(o.events[1], "P7");
    EXPECT_EQ(std::count(o.events.begin(), o.events.end(), "L"), 1);
    EXPECT_EQ(std::count(o.events.begin(), o.events.end(), "U"), 0);
}

// ---------------------------------------------------------------------------------------------
// TST-202 — invalid datagrams IN-08 (REQ-13, ER-21)
// ---------------------------------------------------------------------------------------------

TEST(Tst202, InvalidDatagramsAreCountedAndNotDelivered) {
    Observer o;
    IpTuner tuner(o.on_packets(), o.on_lock());
    ASSERT_TRUE(tuner.tune(0, kSsrc));
    const std::uint16_t port = tuner.bound_port();
    testhelp::UdpSender sender;

    std::vector<Bytes> invalid;
    auto with = [](const std::function<void(testhelp::RtpHeader&)>& change, int packets) {
        testhelp::RtpHeader h;
        h.ssrc = kSsrc;
        change(h);
        std::vector<Packet> ts;
        for (int k = 0; k < packets; ++k) ts.push_back(testhelp::packet(256, static_cast<std::uint8_t>(k), false));
        return testhelp::rtp_datagram(h, ts);
    };
    // IN-08: V = 1; PT = 96; another SSRC; payloads of 100 bytes, 0 bytes and 8 x 188 bytes.
    invalid.push_back(with([](testhelp::RtpHeader& h) { h.byte0 = 0x40; }, 7));
    invalid.push_back(with([](testhelp::RtpHeader& h) { h.payload_type = 96; }, 7));
    invalid.push_back(with([](testhelp::RtpHeader& h) { h.ssrc = kSsrc + 1; }, 7));
    Bytes p100 = with([](testhelp::RtpHeader&) {}, 1);
    p100.resize(12 + 100);
    invalid.push_back(p100);
    invalid.push_back(with([](testhelp::RtpHeader&) {}, 0));
    invalid.push_back(with([](testhelp::RtpHeader&) {}, 8));
    // Further header errors of IF-EXT-05 (12-byte header without padding, extension, CSRC) and sizes.
    invalid.push_back(with([](testhelp::RtpHeader& h) { h.byte0 = 0xA0; }, 7));  // P = 1
    invalid.push_back(with([](testhelp::RtpHeader& h) { h.byte0 = 0x90; }, 7));  // X = 1
    invalid.push_back(with([](testhelp::RtpHeader& h) { h.byte0 = 0x81; }, 7));  // CC = 1
    Bytes plus_one = with([](testhelp::RtpHeader&) {}, 7);
    plus_one.push_back(0x47);
    invalid.push_back(plus_one);
    invalid.push_back(Bytes(5, 0x80));  // shorter than the header
    Bytes too_long(3000, 0);            // larger than the datagram buffer B2
    too_long[0] = 0x80;
    too_long[1] = 33;
    invalid.push_back(too_long);
    // IN-08: 1 000 datagrams of random bytes.
    std::mt19937 rng(202);
    for (int i = 0; i < 1000; ++i) {
        Bytes d(1 + rng() % 1600);
        for (auto& b : d) b = static_cast<std::uint8_t>(rng());
        invalid.push_back(d);
    }

    for (std::size_t i = 0; i < invalid.size(); ++i) {
        sender.send(port, invalid[i]);
        if (i % 50 == 49) {  // stay below the socket buffer
            ASSERT_TRUE(wait_processed(tuner, i + 1));
        }
    }
    ASSERT_TRUE(wait_processed(tuner, invalid.size()));
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_EQ(tuner.stats().rtp_invalid, invalid.size());
    EXPECT_EQ(tuner.stats().rtp_packets, 0u);
    EXPECT_EQ(o.packet_count(), 0u);
    {
        std::lock_guard<std::mutex> lock(o.mutex);
        EXPECT_TRUE(o.events.empty()) << "callback for invalid datagrams";
    }

    // Then one valid datagram: Locked. (The M bit is not checked.)
    Bytes valid = valid_datagram(1, kSsrc, 0);
    valid[1] = static_cast<std::uint8_t>(0x80 | 33);
    sender.send(port, valid);
    ASSERT_TRUE(o.wait_for([&o] { return o.packets.size() == 7; }));
    std::lock_guard<std::mutex> lock(o.mutex);
    EXPECT_EQ(o.events.front(), "L");
}

// ---------------------------------------------------------------------------------------------
// TST-203 — sequence gaps IN-09 and the wrap 65 535 -> 0 (REQ-13, ER-22)
// ---------------------------------------------------------------------------------------------

TEST(Tst203, LostDatagramsAreCountedAndTheWrapIsNoGap) {
    Observer o;
    IpTuner tuner(o.on_packets(), o.on_lock());
    ASSERT_TRUE(tuner.tune(0, kSsrc));
    testhelp::UdpSender sender;

    // IN-09: every 50th of 501 datagrams is not sent (numbers 49, 99, …, 499): 10 omitted.
    std::uint64_t sent = 0;
    for (int i = 0; i <= 500; ++i) {
        if (i % 50 == 49) continue;
        sender.send(tuner.bound_port(), valid_datagram(static_cast<std::uint16_t>(30000 + i), kSsrc, i));
        if (++sent % 50 == 0) {
            ASSERT_TRUE(wait_processed(tuner, sent));
        }
    }
    ASSERT_TRUE(wait_processed(tuner, sent));
    EXPECT_EQ(tuner.stats().rtp_packets, sent);
    EXPECT_EQ(tuner.stats().rtp_lost, 10u);

    // A second run (new tuning: statistics reset) with sequence numbers 65 530 -> 5 without gaps.
    ASSERT_TRUE(tuner.tune(0, kSsrc));
    for (int i = 0; i < 12; ++i) {
        sender.send(tuner.bound_port(), valid_datagram(static_cast<std::uint16_t>(65530 + i), kSsrc, i));
    }
    ASSERT_TRUE(wait_processed(tuner, 12));
    EXPECT_EQ(tuner.stats().rtp_packets, 12u);
    EXPECT_EQ(tuner.stats().rtp_lost, 0u);

    // A late (older) datagram is passed on and is not a gap; the next forward gap still counts.
    ASSERT_TRUE(tuner.tune(0, kSsrc));
    for (std::uint16_t seq : {100, 101, 99, 102, 105}) sender.send(tuner.bound_port(), valid_datagram(seq, kSsrc, 0));
    ASSERT_TRUE(wait_processed(tuner, 5));
    EXPECT_EQ(tuner.stats().rtp_packets, 5u);
    EXPECT_EQ(tuner.stats().rtp_lost, 2u);  // 103, 104

    // Gaps right after and across the wrap are counted: 65 534, (65 535, 0 missing), 1, 2, (3 missing), 4.
    ASSERT_TRUE(tuner.tune(0, kSsrc));
    for (std::uint16_t seq : {65534, 1, 2, 4}) sender.send(tuner.bound_port(), valid_datagram(seq, kSsrc, 0));
    ASSERT_TRUE(wait_processed(tuner, 4));
    EXPECT_EQ(tuner.stats().rtp_lost, 3u);
}

// ---------------------------------------------------------------------------------------------
// TST-204 — Unlocked 3 000 ms ± 150 ms after the last datagram, Locked again (REQ-13)
// ---------------------------------------------------------------------------------------------

TEST(Tst204, UnlockedAfter3sWithoutDatagramsAndLockedAgain) {
    Observer o;
    IpTuner tuner(o.on_packets(), o.on_lock());
    ASSERT_TRUE(tuner.tune(0, kSsrc));
    testhelp::UdpSender sender;
    for (int i = 0; i < 20; ++i) {
        sender.send(tuner.bound_port(), valid_datagram(static_cast<std::uint16_t>(i), kSsrc, i));
        std::this_thread::sleep_for(milliseconds(3));
    }
    ASSERT_TRUE(wait_processed(tuner, 20));
    const auto last_sent = steady_clock::now();  // after the last datagram was processed
    ASSERT_TRUE(o.wait_for([&o] { return o.lock_changes.size() >= 2; }, milliseconds(4000)));
    {
        std::lock_guard<std::mutex> lock(o.mutex);
        ASSERT_FALSE(o.lock_changes[1].first);
        const auto ms = std::chrono::duration_cast<milliseconds>(o.lock_changes[1].second - last_sent).count();
        std::printf("TST-204 measured: Unlocked %lld ms after the last datagram (limit 3000 +/- 150 ms)\n",
                    static_cast<long long>(ms));
        EXPECT_GE(ms, 2850);
        EXPECT_LE(ms, 3150);
    }
    const auto resend = steady_clock::now();
    sender.send(tuner.bound_port(), valid_datagram(20, kSsrc, 20));
    ASSERT_TRUE(o.wait_for([&o] { return o.lock_changes.size() >= 3; }));
    std::lock_guard<std::mutex> lock(o.mutex);
    EXPECT_TRUE(o.lock_changes[2].first);
    EXPECT_LT(o.lock_changes[2].second - resend, milliseconds(100));
    const std::vector<std::string> lock_events = [&o] {
        std::vector<std::string> out;
        for (const auto& e : o.events) {
            if (e == "L" || e == "U") out.push_back(e);
        }
        return out;
    }();
    EXPECT_EQ(lock_events, (std::vector<std::string>{"L", "U", "L"}));
}

// ---------------------------------------------------------------------------------------------
// TST-205 — no callback after untune(); re-tune with another SSRC (REQ-14, rule 4)
// ---------------------------------------------------------------------------------------------

TEST(Tst205, NoCallbackAfterUntuneAndOnlyTheNewSsrcAfterRetune) {
    Observer o;
    IpTuner tuner(o.on_packets(), o.on_lock());
    ASSERT_TRUE(tuner.tune(0, kSsrc));
    const std::uint16_t port = tuner.bound_port();
    testhelp::UdpSender sender;

    // Sender at 380 datagrams/s with SSRC S.
    std::atomic<bool> sending{true};
    std::thread stream([&] {
        int i = 0;
        auto next = steady_clock::now();
        while (sending) {
            sender.send(port, valid_datagram(static_cast<std::uint16_t>(i), kSsrc, i));
            ++i;
            next += std::chrono::microseconds(2632);
            std::this_thread::sleep_until(next);
        }
    });
    ASSERT_TRUE(o.wait_for([&o] { return o.packets.size() >= 700; }));
    tuner.untune();
    o.untuned = true;  // set after the return
    std::this_thread::sleep_for(milliseconds(500));
    EXPECT_EQ(o.callbacks_after_untune.load(), 0);

    // The same with a callback of 2 ms (the receive thread is busy most of the time), 5 times.
    o.callback_delay_ms = 2;
    for (int round = 0; round < 5; ++round) {
        o.untuned = false;
        ASSERT_TRUE(tuner.tune(port, kSsrc));
        const std::size_t base = o.packet_count();
        ASSERT_TRUE(o.wait_for([&o, base] { return o.packets.size() >= base + 140; }));
        tuner.untune();
        o.untuned = true;
        std::this_thread::sleep_for(milliseconds(100));
    }
    o.callback_delay_ms = 0;
    sending = false;
    stream.join();
    EXPECT_EQ(o.callbacks_after_untune.load(), 0);

    // Re-tune the same port with SSRC S2; datagrams of S and S2 alternate: only S2 is delivered.
    o.untuned = false;
    const std::size_t before = o.packet_count();
    const std::uint32_t s2 = 0x5EC0D002;
    ASSERT_TRUE(tuner.tune(port, s2));
    for (int i = 0; i < 100; ++i) {
        const bool new_ssrc = i % 2 == 1;
        sender.send(port, valid_datagram(static_cast<std::uint16_t>(i), new_ssrc ? s2 : kSsrc, i, new_ssrc ? 0x22 : 0x11));
        if (i % 20 == 19) {
            ASSERT_TRUE(wait_processed(tuner, static_cast<std::uint64_t>(i + 1)));
        }
    }
    ASSERT_TRUE(wait_processed(tuner, 100));
    EXPECT_EQ(tuner.stats().rtp_packets, 50u);
    EXPECT_EQ(tuner.stats().rtp_invalid, 50u);  // S counted as invalid
    std::lock_guard<std::mutex> lock(o.mutex);
    ASSERT_EQ(o.packets.size() - before, 350u);
    for (std::size_t i = before; i < o.packets.size(); ++i) EXPECT_EQ(o.packets[i][4 + 10], 0x22) << i;
}

// ---------------------------------------------------------------------------------------------
// TST-206 — port occupied; SO_RCVBUF of 1 MiB requested and the effective value logged (ER-26, B1)
// ---------------------------------------------------------------------------------------------

TEST(Tst206, OccupiedPortFailsAndReceiveBufferIsLogged) {
    std::vector<std::string> log;
    std::mutex log_mutex;
    Observer o;
    IpTuner tuner(o.on_packets(), o.on_lock(), [&](hal::LogLevel, const char* module, const std::string& m) {
        std::lock_guard<std::mutex> lock(log_mutex);
        log.push_back(std::string("[") + module + "] " + m);
    });

    // A port occupied by another socket of the test.
    const int other = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(other, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    getsockname(other, reinterpret_cast<sockaddr*>(&addr), &len);
    const std::uint16_t occupied = ntohs(addr.sin_port);
    EXPECT_FALSE(tuner.tune(occupied, kSsrc));
    EXPECT_EQ(tuner.bound_port(), 0);
    close(other);

    ASSERT_TRUE(tuner.tune(0, kSsrc));
    EXPECT_GT(tuner.effective_receive_buffer(), 0);
    std::lock_guard<std::mutex> lock(log_mutex);
    for (const std::string& line : log) std::printf("TST-206 log: %s\n", line.c_str());  // LOG evidence (5.1.6)
    ASSERT_EQ(log.size(), 2u);
    EXPECT_NE(log[0].find("[HAL-TUN] cannot open UDP port 127.0.0.1:" + std::to_string(occupied)), std::string::npos)
        << log[0];
    EXPECT_NE(log[1].find("SO_RCVBUF requested 1048576 effective " + std::to_string(tuner.effective_receive_buffer())),
              std::string::npos)
        << log[1];
}

// Rule 5 of Section 3.9.1: tune() and untune() from the tuner's own callbacks would make the receive
// thread join itself (an exception); they are refused and logged, and reception continues.
TEST(Tst205, TuneAndUntuneFromATunerCallbackAreRefused) {
    std::atomic<int> refused{0};
    std::atomic<int> packets{0};
    std::atomic<int> tune_results_false{0};
    IpTuner* self = nullptr;
    std::atomic<bool> ready{false};
    IpTuner tuner(
        [&](std::uint8_t*, std::size_t n) {
            while (!ready) std::this_thread::yield();
            self->untune();
            packets += static_cast<int>(n);
        },
        [&](bool) {
            while (!ready) std::this_thread::yield();
            if (!self->tune(0, kSsrc)) ++tune_results_false;
        },
        [&](hal::LogLevel level, const char*, const std::string& message) {
            if (level == hal::LogLevel::Error && message.find("called from a tuner callback: refused") != std::string::npos) {
                ++refused;
            }
        });
    self = &tuner;
    ready = true;
    ASSERT_TRUE(tuner.tune(0, kSsrc));
    testhelp::UdpSender sender;
    std::uint8_t cc = 0;
    for (std::uint16_t seq = 1; seq <= 3; ++seq) {
        std::vector<Packet> block;
        for (int k = 0; k < 7; ++k) block.push_back(testhelp::packet(256, cc++, false));
        testhelp::RtpHeader h;
        h.sequence = seq;
        h.ssrc = kSsrc;
        sender.send(tuner.bound_port(), testhelp::rtp_datagram(h, block));
        ASSERT_TRUE(wait_processed(tuner, seq));
    }
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_EQ(packets.load(), 21);           // still tuned after every refused untune()
    EXPECT_EQ(tune_results_false.load(), 1);  // Locked once
    EXPECT_EQ(refused.load(), 3 + 1);
    tuner.untune();
}
