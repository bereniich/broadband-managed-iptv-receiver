// Unit tests of HAL-DMX (Section 5.3.3): TST-211 – TST-214.
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "demux.hpp"
#include "ts_helper.hpp"

namespace {

using hal::Demux;
using testhelp::Bytes;
using testhelp::Packet;

// Records the deliveries of one filter.
struct Recorder {
    std::vector<Bytes> data;
    std::vector<bool> scrambled;
    Demux::DataCallback callback() {
        return [this](const std::uint8_t* d, std::size_t len, bool s) {
            data.emplace_back(d, d + len);
            scrambled.push_back(s);
        };
    }
};

void feed(Demux& demux, std::vector<Packet> packets) {
    for (Packet& p : packets) demux.feed(p.data(), 1);
}

Bytes bytes(const Packet& p) { return Bytes(p.begin(), p.end()); }

Packet video_packet(std::uint8_t cc, std::uint8_t marker) {
    return testhelp::packet(256, cc, false, Bytes(184, marker));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// TST-211 — sync byte and continuity counter (REQ-15, ER-23)
// ---------------------------------------------------------------------------------------------

TEST(Tst211, SyncErrorsDroppedAndCcErrorsCountedButDelivered) {
    Demux demux;
    Recorder r;
    const auto id = demux.add_filter(Demux::FilterType::Ts, 256, -1, r.callback());
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(demux.start_filter(id));

    std::vector<Packet> in;
    in.push_back(video_packet(0, 1));
    in.push_back(video_packet(1, 2));
    Packet bad = video_packet(2, 3);
    bad[0] = 0x46;  // IN-10: sync byte 0x46
    in.push_back(bad);
    in.push_back(video_packet(2, 4));
    in.push_back(video_packet(5, 5));  // IN-10: continuity counters 3 and 4 skipped -> 1 error
    in.push_back(video_packet(5, 5));  // a repeated packet keeps the counter once: no error
    in.push_back(testhelp::adaptation_only_packet(256, 5));  // no payload: the counter does not increment
    in.push_back(video_packet(6, 6));
    in.push_back(testhelp::discontinuity_packet(256, 12));  // announced jump: no error
    in.push_back(video_packet(13, 7));
    in.push_back(video_packet(15, 8));                       // 14 skipped -> 1 error
    in.push_back(video_packet(0, 9));                        // 15 -> 0 wraps: no error
    in.push_back(testhelp::packet(0x1FFF, 3, false));        // null packets are not checked
    in.push_back(testhelp::packet(0x1FFF, 9, false));
    feed(demux, in);

    const Demux::Stats s = demux.stats();
    EXPECT_EQ(s.ts_packets, in.size());
    EXPECT_EQ(s.sync_errors, 1u);
    EXPECT_EQ(s.cc_errors, 2u);
    // Every packet of PID 256 except the one without sync byte is delivered, in order, unchanged.
    std::vector<Bytes> expected;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (i != 2 && i < 12) expected.push_back(bytes(in[i]));
    }
    EXPECT_EQ(r.data, expected);

    demux.reset_stats();
    EXPECT_EQ(demux.stats().ts_packets, 0u);
    feed(demux, {video_packet(9, 1)});  // first packet after the reset: no reference, no error
    EXPECT_EQ(demux.stats().cc_errors, 0u);
}

TEST(Tst211, ScrambledFlagFollowsTransportScramblingControl) {
    Demux demux;
    Recorder r;
    const auto id = demux.add_filter(Demux::FilterType::Ts, 256, -1, r.callback());
    demux.start_filter(id);
    Packet p0 = video_packet(0, 1);
    Packet p1 = video_packet(1, 1);
    p1[3] = static_cast<std::uint8_t>(0x80 | p1[3]);  // '10'
    feed(demux, {p0, p1});
    ASSERT_EQ(r.scrambled.size(), 2u);
    EXPECT_FALSE(r.scrambled[0]);
    EXPECT_TRUE(r.scrambled[1]);
}

// ---------------------------------------------------------------------------------------------
// TST-212 — delivery per PID, one packet per callback, several filters on one PID (REQ-19)
// ---------------------------------------------------------------------------------------------

TEST(Tst212, EachFilterReceivesOnlyItsPid) {
    Demux demux;
    Recorder video_a, video_b, audio, pat, stopped;
    const auto a = demux.add_filter(Demux::FilterType::Ts, 256, -1, video_a.callback());
    const auto b = demux.add_filter(Demux::FilterType::Ts, 256, -1, video_b.callback());
    const auto c = demux.add_filter(Demux::FilterType::Ts, 257, -1, audio.callback());
    const auto d = demux.add_filter(Demux::FilterType::Section, 0, 0x00, pat.callback());
    const auto e = demux.add_filter(Demux::FilterType::Ts, 256, -1, stopped.callback());  // never started
    for (auto id : {a, b, c, d}) ASSERT_TRUE(demux.start_filter(id));
    ASSERT_NE(e, 0u);

    // Invalid filters: PID above 0x1FFF, table_id outside -1 … 255, empty callback.
    EXPECT_EQ(demux.add_filter(Demux::FilterType::Ts, 8192, -1, video_a.callback()), 0u);
    EXPECT_EQ(demux.add_filter(Demux::FilterType::Section, 0, 256, video_a.callback()), 0u);
    EXPECT_EQ(demux.add_filter(Demux::FilterType::Ts, 256, -1, nullptr), 0u);

    const Bytes pat_section = testhelp::section(0x00, 21);
    std::uint8_t cc0 = 0;
    std::vector<Packet> in;
    std::uint8_t cc[8192] = {};
    for (int round = 0; round < 10; ++round) {
        in.push_back(testhelp::packetize(0, cc0, pat_section, {})[0]);
        for (std::uint16_t pid : {256, 257, 4096, 8191, 256}) {
            in.push_back(testhelp::packet(pid, cc[pid]++, false, Bytes(184, static_cast<std::uint8_t>(round))));
        }
    }
    // Delivered as RTP payloads of 7 packets (one feed per datagram).
    for (std::size_t i = 0; i < in.size(); i += 7) {
        std::vector<std::uint8_t> datagram;
        const std::size_t n = std::min<std::size_t>(7, in.size() - i);
        for (std::size_t k = 0; k < n; ++k) datagram.insert(datagram.end(), in[i + k].begin(), in[i + k].end());
        demux.feed(datagram.data(), n);
    }

    auto packets_of = [&in](std::uint16_t pid) {
        std::vector<Bytes> out;
        for (const Packet& p : in) {
            if ((((p[1] & 0x1F) << 8) | p[2]) == pid) out.push_back(bytes(p));
        }
        return out;
    };
    EXPECT_EQ(video_a.data, packets_of(256));  // only PID 256, every packet, 188 bytes each
    EXPECT_EQ(video_b.data, packets_of(256));
    EXPECT_EQ(audio.data, packets_of(257));
    EXPECT_EQ(pat.data, std::vector<Bytes>(10, pat_section));
    EXPECT_TRUE(stopped.data.empty());  // a filter is created stopped
    for (const Bytes& p : video_a.data) EXPECT_EQ(p.size(), 188u);

    // Stopped and removed filters receive nothing more.
    demux.stop_filter(a);
    demux.remove_filter(b);
    const std::size_t a_before = video_a.data.size(), b_before = video_b.data.size();
    feed(demux, {testhelp::packet(256, cc[256]++, false)});
    EXPECT_EQ(video_a.data.size(), a_before);
    EXPECT_EQ(video_b.data.size(), b_before);
    EXPECT_FALSE(demux.start_filter(b));
}

// ---------------------------------------------------------------------------------------------
// TST-213 — section assembly (REQ-16, REQ-17, B3, ER-24)
// ---------------------------------------------------------------------------------------------

TEST(Tst213, HelperCrcHasTheStandardCheckValue) {
    const char* text = "123456789";
    EXPECT_EQ(testhelp::crc32_mpeg2(reinterpret_cast<const std::uint8_t*>(text), 9), 0x0376E6E7u);
}

TEST(Tst213, SectionsAreAssembledByteForByte) {
    Demux demux;
    Recorder all, pmt_only;
    const auto f_all = demux.add_filter(Demux::FilterType::Section, 4096, -1, all.callback());
    const auto f_pmt = demux.add_filter(Demux::FilterType::Section, 4096, 0x02, pmt_only.callback());
    demux.start_filter(f_all);
    demux.start_filter(f_pmt);
    std::uint8_t cc = 0;

    // 1. One section in one packet with pointer_field: 3 bytes of an earlier section come first.
    const Bytes s1 = testhelp::section(0x02, 40, 1);
    Bytes payload = {3, 0xAA, 0xBB, 0xCC};
    payload.insert(payload.end(), s1.begin(), s1.end());
    feed(demux, {testhelp::packet(4096, cc++, true, payload)});

    // 2. One section over 3 packets.
    const Bytes s2 = testhelp::section(0x02, 450, 2);
    const auto p2 = testhelp::packetize(4096, cc, s2, {});
    ASSERT_EQ(p2.size(), 3u);
    feed(demux, p2);

    // 3. Two sections in one packet (a 0x00 section, then a 0x02 section).
    const Bytes s3a = testhelp::section(0x00, 30, 3);
    const Bytes s3b = testhelp::section(0x02, 60, 4);
    Bytes two = s3a;
    two.insert(two.end(), s3b.begin(), s3b.end());
    feed(demux, testhelp::packetize(4096, cc, two, {s3a.size()}));

    // 4. A section whose header continues in the next packet, followed by one starting in the same
    //    packet as the end of the previous one (pointer_field > 0).
    const Bytes s4a = testhelp::section(0x02, 182, 5);  // 1 + 182 = 183 bytes: 1 byte left in packet 1
    const Bytes s4b = testhelp::section(0x02, 300, 6);
    const Bytes s4c = testhelp::section(0x02, 100, 7);
    Bytes stream4 = s4a;
    stream4.insert(stream4.end(), s4b.begin(), s4b.end());
    stream4.insert(stream4.end(), s4c.begin(), s4c.end());
    feed(demux, testhelp::packetize(4096, cc, stream4, {s4a.size(), s4a.size() + s4b.size()}));

    const std::vector<Bytes> expected_all = {s1, s2, s3a, s3b, s4a, s4b, s4c};
    EXPECT_EQ(all.data, expected_all);
    // 5. table_id filter 0x02: the 0x00 section is not delivered.
    const std::vector<Bytes> expected_pmt = {s1, s2, s3b, s4a, s4b, s4c};
    EXPECT_EQ(pmt_only.data, expected_pmt);
    EXPECT_EQ(demux.stats().sections_discarded, 0u);
    EXPECT_EQ(demux.stats().cc_errors, 0u);
}

TEST(Tst213, OversizedSectionIsDiscardedAndAssemblyRestartsAtTheNextUnitStart) {
    std::vector<std::string> log;
    Demux demux([&log](hal::LogLevel, const char* module, const std::string& m) { log.push_back(std::string(module) + " " + m); });
    Recorder r;
    const auto f = demux.add_filter(Demux::FilterType::Section, 0, -1, r.callback());
    demux.start_filter(f);
    std::uint8_t cc = 0;

    // The largest section the 12-bit section_length allows: 4 098 bytes (> B3 = 4 096). The input
    // of TST-213 names 5 000 bytes, which section_length cannot express (reported for spec 1.2).
    const Bytes big = testhelp::section(0x00, 4098, 9);
    const Bytes next = testhelp::section(0x00, 21, 10);
    feed(demux, testhelp::packetize(0, cc, big, {}));
    feed(demux, testhelp::packetize(0, cc, next, {}));
    EXPECT_EQ(r.data, std::vector<Bytes>{next});
    EXPECT_EQ(demux.stats().sections_discarded, 1u);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_NE(log[0].find("HAL-DMX section discarded on PID 0: section longer than 4096 bytes"), std::string::npos)
        << log[0];

    // The largest section that fits B3 (4 096 bytes) is delivered.
    const Bytes fits = testhelp::section(0x00, 4096, 11);
    feed(demux, testhelp::packetize(0, cc, fits, {}));
    EXPECT_EQ(r.data.back(), fits);

    // A continuity gap in the middle of a section discards it; the next section is delivered.
    const Bytes s = testhelp::section(0x00, 400, 12);
    auto packets = testhelp::packetize(0, cc, s, {});
    packets.erase(packets.begin() + 1);  // one packet lost
    feed(demux, packets);
    feed(demux, testhelp::packetize(0, cc, next, {}));
    EXPECT_EQ(r.data.back(), next);
    EXPECT_EQ(demux.stats().sections_discarded, 2u);
    EXPECT_EQ(r.data.size(), 3u);
}

// ---------------------------------------------------------------------------------------------
// TST-214 — no callback after remove_filter returned (Section 3.9.1 rule 4)
// ---------------------------------------------------------------------------------------------

TEST(Tst214, NoCallbackAfterRemoveFilterReturned) {
    Demux demux;
    std::atomic<bool> feeding{true};
    std::thread feeder([&] {
        std::vector<std::uint8_t> block;
        std::uint8_t cc = 0;
        for (int k = 0; k < 7; ++k) {
            const Packet p = testhelp::packet(256, cc++, false);
            block.insert(block.end(), p.begin(), p.end());
        }
        while (feeding) {
            for (int k = 0; k < 7; ++k) block[k * 188 + 3] = static_cast<std::uint8_t>(0x10 | (cc++ & 0x0F));
            demux.feed(block.data(), 7);
        }
    });

    std::atomic<int> violations{0};
    std::atomic<long> deliveries{0};
    for (int i = 0; i < 1000; ++i) {
        auto closed = std::make_shared<std::atomic<bool>>(false);
        const auto id = demux.add_filter(Demux::FilterType::Ts, 256, -1,
                                         [closed, &violations, &deliveries](const std::uint8_t*, std::size_t, bool) {
                                             if (closed->load()) ++violations;
                                             ++deliveries;
                                         });
        ASSERT_NE(id, 0u);
        demux.start_filter(id);
        std::this_thread::yield();
        demux.remove_filter(id);
        closed->store(true);  // set after the return
    }
    feeding = false;
    feeder.join();
    EXPECT_EQ(violations.load(), 0);
    EXPECT_GT(deliveries.load(), 0) << "the filters never received data; the test proves nothing";
}

// Rule 2 and 5 of Section 3.9.1 with rule 4: a demux method called from a filter callback cannot run
// (the filter table is locked by the delivering thread); it is refused and logged instead of
// deadlocking, stats() still answers, and the filter keeps working after the delivery.
TEST(Tst214, CallsFromAFilterCallbackAreRefusedInsteadOfDeadlocking) {
    std::atomic<int> errors{0};
    Demux demux([&errors](hal::LogLevel level, const char* module, const std::string& message) {
        if (level == hal::LogLevel::Error && std::string(module) == "HAL-DMX" &&
            message.find("called from a filter callback: refused") != std::string::npos) {
            ++errors;
        }
    });
    int deliveries = 0;
    std::uint64_t seen_packets = 0;
    Demux::FilterId id = 0;
    std::vector<std::uint8_t> other(188, 0);
    id = demux.add_filter(Demux::FilterType::Ts, 256, -1, [&](const std::uint8_t*, std::size_t, bool) {
        ++deliveries;
        EXPECT_EQ(demux.add_filter(Demux::FilterType::Ts, 257, -1, [](const std::uint8_t*, std::size_t, bool) {}), 0u);
        EXPECT_FALSE(demux.start_filter(id));
        demux.stop_filter(id);
        demux.remove_filter(id);
        demux.reset_stats();
        demux.feed(other.data(), 1);
        seen_packets = demux.stats().ts_packets;  // works: the mutex is held by this thread
    });
    ASSERT_TRUE(demux.start_filter(id));
    std::uint8_t cc = 0;
    Packet p = testhelp::packet(256, cc++, false);
    demux.feed(p.data(), 1);
    EXPECT_EQ(deliveries, 1);
    EXPECT_EQ(errors.load(), 6);
    EXPECT_EQ(seen_packets, 1u);
    p = testhelp::packet(256, cc++, false);
    demux.feed(p.data(), 1);  // still added and started
    EXPECT_EQ(deliveries, 2);
    EXPECT_EQ(demux.stats().ts_packets, 2u);
    demux.remove_filter(id);
    p = testhelp::packet(256, cc++, false);
    demux.feed(p.data(), 1);
    EXPECT_EQ(deliveries, 2);
}
