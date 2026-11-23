// TST-101 – TST-104, function level: MPEG-2 CRC-32, parse_pat, parse_pmt (REQ-16, REQ-17).
// The parts of TST-102 and TST-104 that go through the fake demux and AL-PSI (EV-PAT,
// EV-PMT-NOVIDEO, log warnings) are added with the AL-PSI module.
#include <gtest/gtest.h>

#include <string>

#include "ffmpeg_fixtures.hpp"
#include "psi.hpp"
#include "psi_builder.hpp"

namespace {

using al::PsiError;
using testutil::Bytes;

using testutil::kFfmpegPat;
using testutil::kFfmpegPmt;

std::uint32_t crc_of(const std::string& text) {
    return al::crc32_mpeg2(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

// TST-101 (a): one program 1 -> PMT PID 4096.
Bytes pat_a() { return testutil::make_pat({{1, 4096}}); }

// TST-103 (a): PCR 256, 0x1B on 256, 0x0F on 257.
Bytes pmt_a() { return testutil::make_pmt(1, 256, {{0x1B, 256, {}}, {0x0F, 257, {}}}); }

// ---------------------------------------------------------------- TST-101

TEST(Tst101, HelperCrcCheckValue) {
    EXPECT_EQ(testutil::crc32_mpeg2_bitwise(std::string("123456789")), 0x0376E6E7u);
}

TEST(Tst101, HelperBuildsTheSectionsOfFfmpeg) {
    // The helper is checked against the independent tool: same fields give the same bytes.
    EXPECT_EQ(testutil::make_pat({{1, 4096}}, 1), kFfmpegPat);
    EXPECT_EQ(testutil::make_pmt(1, 256, {{0x1B, 256, {}}, {0x0F, 257, {}}}), kFfmpegPmt);
}

TEST(Tst101, ParsePatOneProgram) {
    const Bytes s = pat_a();
    const al::PatResult r = al::parse_pat(s.data(), s.size());
    ASSERT_EQ(r.error, PsiError::None);
    EXPECT_EQ(r.program_number, 1);
    EXPECT_EQ(r.pmt_pid, 4096);
}

TEST(Tst101, ParsePatSkipsProgramZero) {
    const Bytes s = testutil::make_pat({{0, 16}, {2, 768}});   // (b): NIT first, then program 2
    const al::PatResult r = al::parse_pat(s.data(), s.size());
    ASSERT_EQ(r.error, PsiError::None);
    EXPECT_EQ(r.program_number, 2);
    EXPECT_EQ(r.pmt_pid, 768);
}

TEST(Tst101, ParsePatOfFfmpeg) {
    const al::PatResult r = al::parse_pat(kFfmpegPat.data(), kFfmpegPat.size());
    ASSERT_EQ(r.error, PsiError::None);
    EXPECT_EQ(r.program_number, 1);
    EXPECT_EQ(r.pmt_pid, 4096);
    EXPECT_EQ(r.version, 0);
}

// ---------------------------------------------------------------- TST-102 (function level)

TEST(Tst102, CrcCheckValueAndKnownSections) {
    EXPECT_EQ(crc_of("123456789"), 0x0376E6E7u);
    EXPECT_EQ(al::crc32_mpeg2(kFfmpegPat.data(), kFfmpegPat.size() - 4), 0x2AB104B2u);
    EXPECT_EQ(al::crc32_mpeg2(kFfmpegPmt.data(), kFfmpegPmt.size() - 4), 0x2F44B99Bu);
}

TEST(Tst102, InvalidPatVariantsAreRejected) {
    Bytes bad_crc = pat_a();
    bad_crc[bad_crc.size() - 2] ^= 0x01;
    EXPECT_EQ(al::parse_pat(bad_crc.data(), bad_crc.size()).error, PsiError::BadCrc);

    const Bytes bad_table = testutil::make_pat({{1, 4096}}, 1, 0, 0x02);   // valid CRC
    EXPECT_EQ(al::parse_pat(bad_table.data(), bad_table.size()).error, PsiError::BadTableId);

    Bytes too_long = pat_a();
    too_long[2] = static_cast<std::uint8_t>(too_long[2] + 4);   // section_length larger than the data
    EXPECT_EQ(al::parse_pat(too_long.data(), too_long.size()).error, PsiError::BadSectionLength);

    EXPECT_EQ(al::parse_pat(nullptr, 0).error, PsiError::TooShort);
    const Bytes two = {0x00, 0xb0};
    EXPECT_EQ(al::parse_pat(two.data(), two.size()).error, PsiError::TooShort);

    const Bytes valid = pat_a();
    const al::PatResult r = al::parse_pat(valid.data(), valid.size());
    ASSERT_EQ(r.error, PsiError::None);
    EXPECT_EQ(r.pmt_pid, 4096);
}

TEST(Tst102, PatWithoutProgramIsRejected) {
    const Bytes only_nit = testutil::make_pat({{0, 16}});
    EXPECT_EQ(al::parse_pat(only_nit.data(), only_nit.size()).error, PsiError::NoProgram);
    const Bytes empty = testutil::make_pat({});
    EXPECT_EQ(al::parse_pat(empty.data(), empty.size()).error, PsiError::NoProgram);
}

TEST(Tst102, BytesAfterTheSectionAreIgnored) {
    Bytes s = pat_a();
    s.insert(s.end(), 10, 0xFF);   // stuffing after the CRC
    const al::PatResult r = al::parse_pat(s.data(), s.size());
    ASSERT_EQ(r.error, PsiError::None);
    EXPECT_EQ(r.pmt_pid, 4096);
}

// ---------------------------------------------------------------- TST-103

TEST(Tst103, ParsePmtA) {
    const Bytes s = pmt_a();
    const al::PmtResult r = al::parse_pmt(s.data(), s.size());
    ASSERT_EQ(r.error, PsiError::None);
    EXPECT_EQ(r.pcr_pid, 256);
    ASSERT_TRUE(r.video.has_value());
    EXPECT_EQ(r.video->pid, 256);
    EXPECT_EQ(r.video->stream_type, 0x1B);
    ASSERT_TRUE(r.audio.has_value());
    EXPECT_EQ(r.audio->pid, 257);
    EXPECT_EQ(r.audio->stream_type, 0x0F);
}

TEST(Tst103, ParsePmtB) {
    const Bytes s = testutil::make_pmt(2, 1025, {{0x1B, 1025, {}}, {0x0F, 1026, {}}});
    const al::PmtResult r = al::parse_pmt(s.data(), s.size());
    ASSERT_EQ(r.error, PsiError::None);
    EXPECT_EQ(r.program_number, 2);
    EXPECT_EQ(r.pcr_pid, 1025);
    ASSERT_TRUE(r.video && r.audio);
    EXPECT_EQ(r.video->pid, 1025);
    EXPECT_EQ(r.audio->pid, 1026);
}

TEST(Tst103, ParsePmtCSkipsOtherStreamTypes) {
    // (c): 0x06 with a descriptor (ISO 639 language descriptor, tag 0x0A), 0x0F on 300, 0x1B on 301;
    // a program_info descriptor is present as well.
    const Bytes s = testutil::make_pmt(1, 301,
                                       {{0x06, 299, {0x0A, 0x04, 'e', 'n', 'g', 0x00}},
                                        {0x0F, 300, {}},
                                        {0x1B, 301, {}}},
                                       {0x0E, 0x03, 0xC0, 0x00, 0x00});
    const al::PmtResult r = al::parse_pmt(s.data(), s.size());
    ASSERT_EQ(r.error, PsiError::None);
    ASSERT_TRUE(r.video && r.audio);
    EXPECT_EQ(r.video->pid, 301);
    EXPECT_EQ(r.audio->pid, 300);
}

TEST(Tst103, ParsePmtOfFfmpeg) {
    const al::PmtResult r = al::parse_pmt(kFfmpegPmt.data(), kFfmpegPmt.size());
    ASSERT_EQ(r.error, PsiError::None);
    EXPECT_EQ(r.program_number, 1);
    EXPECT_EQ(r.pcr_pid, 256);
    ASSERT_TRUE(r.video && r.audio);
    EXPECT_EQ(r.video->pid, 256);
    EXPECT_EQ(r.video->stream_type, 0x1B);
    EXPECT_EQ(r.audio->pid, 257);
    EXPECT_EQ(r.audio->stream_type, 0x0F);
}

// ---------------------------------------------------------------- TST-104 (function level)

TEST(Tst104, PmtWithoutVideo) {
    const Bytes s = testutil::make_pmt(1, 256, {{0x0F, 256, {}}});   // (d)
    const al::PmtResult r = al::parse_pmt(s.data(), s.size());
    ASSERT_EQ(r.error, PsiError::None);   // a valid section; the session maps it to EV-PMT-NOVIDEO
    EXPECT_FALSE(r.video.has_value());
    ASSERT_TRUE(r.audio.has_value());
    EXPECT_EQ(r.audio->pid, 256);
}

TEST(Tst104, PmtWithoutAudio) {
    const Bytes s = testutil::make_pmt(1, 256, {{0x1B, 256, {}}});   // (e)
    const al::PmtResult r = al::parse_pmt(s.data(), s.size());
    ASSERT_EQ(r.error, PsiError::None);
    ASSERT_TRUE(r.video.has_value());
    EXPECT_EQ(r.video->pid, 256);
    EXPECT_FALSE(r.audio.has_value());
}

TEST(Tst104, PmtWithChangedCrcIsDiscarded) {
    Bytes s = pmt_a();
    s[s.size() - 1] ^= 0x80;
    EXPECT_EQ(al::parse_pmt(s.data(), s.size()).error, PsiError::BadCrc);
}

TEST(Tst104, PmtWithLengthsOutsideTheSectionIsMalformed) {
    // ES_info_length points beyond the end of the section (CRC recomputed, so only the length is wrong).
    const Bytes es_too_long = testutil::make_pmt(1, 256, {{0x1B, 256, {}}});
    Bytes patched = es_too_long;
    patched.resize(patched.size() - 4);
    patched[patched.size() - 1] = 0x10;   // ES_info_length of the last stream: 16 bytes, none present
    const std::uint32_t crc = testutil::crc32_mpeg2_bitwise(patched.data(), patched.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        patched.push_back(static_cast<std::uint8_t>((crc >> shift) & 0xFF));
    }
    EXPECT_EQ(al::parse_pmt(patched.data(), patched.size()).error, PsiError::Malformed);

    Bytes bad_table = pmt_a();
    bad_table[0] = 0x00;
    EXPECT_EQ(al::parse_pmt(bad_table.data(), bad_table.size()).error, PsiError::BadTableId);
}

}  // namespace
