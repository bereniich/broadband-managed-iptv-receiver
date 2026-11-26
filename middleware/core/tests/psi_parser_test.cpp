// AL-PSI module tests (step 4): TST-102 and TST-104 through the fake demux, the PSI part of TST-105 and
// TST-106. The TS filters (REQ-19, AL-DSC) and stream_info (T-12, AL-SES) are added in later steps.
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "content.hpp"
#include "fakes/call_log.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_demux.hpp"
#include "fakes/fake_logger.hpp"
#include "psi_builder.hpp"
#include "psi_parser.hpp"
#include "task_queue.hpp"
#include "ts_builder.hpp"

namespace {

using testutil::Bytes;

// The test plays AL-SES: on EV-PAT it opens the PMT filter (T-11).
struct Harness {
    fakes::CallLog calls;
    fakes::FakeDemux demux{&calls};
    fakes::FakeClock clock;
    fakes::FakeLogger logger;
    al::TaskQueue queue{clock, logger};
    al::PsiParser psi{demux, logger, queue};

    std::vector<std::uint16_t> pats;
    std::vector<al::StreamDescription> pmts;
    int novideo = 0;
    std::uint8_t cc_pat = 0;
    std::uint8_t cc_pmt = 0;

    bool start(std::uint64_t session = 1, std::uint64_t generation = 1) {
        al::PsiParser::Events events;
        events.on_pat = [this](std::uint16_t pid) {
            pats.push_back(pid);
            psi.start_pmt(pid);
        };
        events.on_pmt = [this](const al::StreamDescription& d) { pmts.push_back(d); };
        events.on_pmt_novideo = [this] { ++novideo; };
        return psi.start_pat(session, generation, events);
    }

    // Feeds a section on its PID and runs the queue (the worker).
    void feed_section(std::uint16_t pid, const Bytes& section) {
        std::uint8_t& cc = pid == 0 ? cc_pat : cc_pmt;
        const std::vector<Bytes> packets = testutil::packetize_section(pid, section, cc);
        cc = static_cast<std::uint8_t>((cc + packets.size()) & 0x0F);
        demux.feed(testutil::join(packets));
        queue.run_pending();
    }

    // Feeds a content file packet by packet, running the queue after each packet, so that the PMT
    // filter is open before the next PMT arrives (as on the real worker).
    bool feed_content(const std::string& name, double seconds) {
        const std::string path = testutil::content_path(name);
        if (!testutil::content_exists(path)) return false;
        const std::size_t count = testutil::packets_for_seconds(seconds);
        for (std::size_t i = 0; i < count; ++i) {
            if (demux.feed_file(path, i, 1) != 1) break;
            queue.run_pending();
        }
        return true;
    }

    std::size_t warnings() const { return logger.count(pal::LogLevel::Warn); }
};

Bytes pat_a() { return testutil::make_pat({{1, 4096}}); }
Bytes pmt_a(std::uint8_t version = 0) {
    return testutil::make_pmt(1, 256, {{0x1B, 256, {}}, {0x0F, 257, {}}}, {}, version);
}

// ---------------------------------------------------------------- TST-102 (through the fake demux)

TEST(Tst102, InvalidPatSectionsThroughTheFilter) {
    Harness h;
    ASSERT_TRUE(h.start());

    // (1) one CRC byte changed.
    Bytes bad_crc = pat_a();
    bad_crc[bad_crc.size() - 1] ^= 0x01;
    h.feed_section(0, bad_crc);
    // (2) section_length larger than the data: the demux completes the section with the following
    // stuffing bytes, so AL-PSI receives a section whose CRC does not match.
    Bytes too_long = pat_a();
    too_long[2] = static_cast<std::uint8_t>(too_long[2] + 4);
    h.feed_section(0, too_long);
    // (3) table_id 0x02 with a valid CRC: not delivered by the PAT filter (table_id 0x00, REQ-16,
    // Section 3.9.2), so AL-PSI sees nothing (decision P1, 2026-11-23).
    h.feed_section(0, testutil::make_pat({{1, 4096}}, 1, 0, 0x02));

    EXPECT_TRUE(h.pats.empty());   // no EV-PAT for the invalid sections
    EXPECT_EQ(h.warnings(), 2u);   // one warning per delivered invalid section
    EXPECT_EQ(h.logger.count_containing("discarded PAT section"), 2u);

    h.feed_section(0, pat_a());
    ASSERT_EQ(h.pats.size(), 1u);
    EXPECT_EQ(h.pats[0], 4096);
    EXPECT_TRUE(h.logger.contains("PAT program=1 pmt_pid=4096 session=1 generation=1"));
}

// ---------------------------------------------------------------- TST-104 (through the fake demux)

TEST(Tst104, PmtEventsThroughTheFilter) {
    Harness h;

    // (d) only 0x0F: EV-PMT-NOVIDEO.
    ASSERT_TRUE(h.start(1, 1));
    h.feed_section(0, pat_a());
    h.feed_section(4096, testutil::make_pmt(1, 256, {{0x0F, 256, {}}}));
    EXPECT_EQ(h.novideo, 1);
    EXPECT_TRUE(h.pmts.empty());
    EXPECT_TRUE(h.logger.contains("PMT pcr_pid=256 video=none audio=256/0x0F session=1 generation=1"));

    // (e) only 0x1B: video only, the stream is accepted.
    ASSERT_TRUE(h.start(2, 2));
    h.feed_section(0, pat_a());
    h.feed_section(4096, testutil::make_pmt(1, 256, {{0x1B, 256, {}}}));
    ASSERT_EQ(h.pmts.size(), 1u);
    EXPECT_EQ(h.pmts[0].video_pid, 256);
    EXPECT_FALSE(h.pmts[0].audio_pid.has_value());
    EXPECT_TRUE(h.logger.contains("PMT pcr_pid=256 video=256/0x1B audio=none session=2 generation=2"));

    // (a) with one CRC byte changed: discarded, no event, one warning.
    ASSERT_TRUE(h.start(3, 3));
    h.feed_section(0, pat_a());
    const std::size_t warnings_before = h.warnings();
    Bytes bad = pmt_a();
    bad[bad.size() - 2] ^= 0x10;
    h.feed_section(4096, bad);
    EXPECT_EQ(h.pmts.size(), 1u);
    EXPECT_EQ(h.novideo, 1);
    EXPECT_EQ(h.warnings(), warnings_before + 1);
    EXPECT_TRUE(h.logger.contains("discarded PMT section: CRC-32 mismatch session=3 generation=3"));
}

// ---------------------------------------------------------------- TST-105 (PSI part)

TEST(Tst105, SectionFilterCallsAndRepetitions) {
    Harness h;
    ASSERT_TRUE(h.start());
    h.feed_section(0, pat_a());
    h.feed_section(4096, pmt_a());
    const std::vector<std::string> after_first = h.calls.entries();
    for (int i = 0; i < 10; ++i) {
        h.feed_section(0, pat_a());
        h.feed_section(4096, pmt_a());
    }

    EXPECT_EQ(after_first, (std::vector<std::string>{"open_filter section pid=0 table_id=0 -> 1", "start_filter 1",
                                                     "open_filter section pid=4096 table_id=2 -> 2",
                                                     "start_filter 2"}));
    EXPECT_EQ(h.calls.entries(), after_first);   // repetitions cause no further filter call
    EXPECT_EQ(h.pats.size(), 1u);
    ASSERT_EQ(h.pmts.size(), 1u);
    const al::StreamDescription& d = h.pmts[0];
    EXPECT_EQ(d.program_number, 1);
    EXPECT_EQ(d.pmt_pid, 4096);
    EXPECT_EQ(d.pcr_pid, 256);
    EXPECT_EQ(d.video_pid, 256);
    EXPECT_EQ(d.video_stream_type, 0x1B);
    EXPECT_EQ(d.audio_pid, std::optional<std::uint16_t>(257));
    EXPECT_EQ(d.audio_stream_type, std::optional<std::uint8_t>(0x0F));
    EXPECT_EQ(h.warnings(), 0u);
}

TEST(Tst105, AltPidStructureFromSections) {
    Harness h;   // (b) of TST-103: the PIDs of content/test/svc-2.altpid.ts (Section 5.2.1)
    ASSERT_TRUE(h.start());
    h.feed_section(0, testutil::make_pat({{2, 768}}));
    h.feed_section(768, testutil::make_pmt(2, 1025, {{0x1B, 1025, {}}, {0x0F, 1026, {}}}));
    EXPECT_NE(h.calls.find("open_filter section pid=768 table_id=2"), fakes::CallLog::npos);
    ASSERT_EQ(h.pmts.size(), 1u);
    EXPECT_EQ(h.pmts[0].program_number, 2);
    EXPECT_EQ(h.pmts[0].pmt_pid, 768);
    EXPECT_EQ(h.pmts[0].pcr_pid, 1025);
    EXPECT_EQ(h.pmts[0].video_pid, 1025);
    EXPECT_EQ(h.pmts[0].audio_pid, std::optional<std::uint16_t>(1026));
}

TEST(Tst105, VersionChangeIgnored) {
    Harness h;
    ASSERT_TRUE(h.start());
    h.feed_section(0, pat_a());
    h.feed_section(4096, pmt_a(0));
    for (int i = 0; i < 3; ++i) h.feed_section(4096, pmt_a(1));
    h.feed_section(4096, pmt_a(2));
    EXPECT_EQ(h.pmts.size(), 1u);   // the description of version 0 is kept
    EXPECT_EQ(h.logger.count_containing("PMT version changed"), 2u);
    EXPECT_TRUE(h.logger.contains("PMT version changed 0 -> 1, ignored"));
    EXPECT_TRUE(h.logger.contains("PMT version changed 1 -> 2, ignored"));
}

TEST(Tst105, OldGenerationAndCloseDiscard) {
    Harness h;
    ASSERT_TRUE(h.start(1, 1));
    // A section of generation 1 is queued, then the attempt is replaced before the worker runs.
    h.demux.feed(testutil::join(testutil::packetize_section(0, pat_a(), 0)));
    ASSERT_TRUE(h.start(1, 2));   // closes the filter of generation 1 first
    h.queue.run_pending();
    EXPECT_TRUE(h.pats.empty());
    EXPECT_NE(h.calls.find("close_filter 1"), fakes::CallLog::npos);

    // close(): both filters closed, no event afterwards.
    h.feed_section(0, pat_a());
    ASSERT_EQ(h.pats.size(), 1u);
    h.demux.feed(testutil::join(testutil::packetize_section(4096, pmt_a(), 0)));
    h.psi.close();
    h.queue.run_pending();
    EXPECT_TRUE(h.pmts.empty());
    EXPECT_TRUE(h.demux.open_filters().empty());
    EXPECT_FALSE(h.psi.start_pmt(4096));   // no attempt running
}

TEST(Tst105, OpenFilterFailure) {
    Harness h;
    h.demux.fail_open_filters(true);
    EXPECT_FALSE(h.start());
    EXPECT_EQ(h.logger.count(pal::LogLevel::Error), 1u);
    EXPECT_TRUE(h.logger.contains("cannot open the PAT section filter on PID 0"));
    EXPECT_TRUE(h.demux.open_filters().empty());
}

// ---------------------------------------------------------------- TST-105, TST-106 with content

// Expected values: the reference and test sets of Section 5.2.1, whose PIDs are checked with
// ffprobe by TST-301. 0.5 s of content contains at least 5 PAT / PMT pairs (period 0.09 s).
struct ContentCase {
    const char* file;
    std::uint16_t program, pmt_pid, video, audio;
};

const ContentCase kContentCases[] = {
    {"svc-1.ts", 1, 4096, 256, 257},
    {"test/svc-2.altpid.ts", 2, 768, 1025, 1026},
};

TEST(Tst105, ContentFiles) {
    for (const ContentCase& c : kContentCases) {
        SCOPED_TRACE(c.file);
        Harness h;
        ASSERT_TRUE(h.start());
        if (!h.feed_content(c.file, 0.5)) continue;
        ASSERT_EQ(h.pats.size(), 1u);
        EXPECT_EQ(h.pats[0], c.pmt_pid);
        ASSERT_EQ(h.pmts.size(), 1u);
        EXPECT_EQ(h.pmts[0].program_number, c.program);
        EXPECT_EQ(h.pmts[0].pmt_pid, c.pmt_pid);
        EXPECT_EQ(h.pmts[0].pcr_pid, c.video);   // the video PID is also the PCR PID
        EXPECT_EQ(h.pmts[0].video_pid, c.video);
        EXPECT_EQ(h.pmts[0].audio_pid, std::optional<std::uint16_t>(c.audio));
        EXPECT_EQ(h.calls.entries().size(), 4u);   // two section filters, no further call
    }
}

TEST(Tst106, PsiLogLines) {
    for (const ContentCase& c : kContentCases) {
        SCOPED_TRACE(c.file);
        Harness h;
        ASSERT_TRUE(h.start());
        if (!h.feed_content(c.file, 0.5)) continue;
        const std::string pat = "PAT program=" + std::to_string(c.program) + " pmt_pid=" + std::to_string(c.pmt_pid);
        const std::string pmt = "PMT pcr_pid=" + std::to_string(c.video) + " video=" + std::to_string(c.video) +
                                "/0x1B audio=" + std::to_string(c.audio) + "/0x0F";
        EXPECT_EQ(h.logger.count_containing(pat), 1u);
        EXPECT_EQ(h.logger.count_containing(pmt), 1u);
        EXPECT_EQ(h.warnings(), 0u);
    }
}

}  // namespace
