// SRV-PKG unit tests (Section 5.3.4): TST-304 – TST-306 (REQ-63, ER-39; WP-APP).
// Expected values come from outside the code under test (Section 5.1.7, rule 5): the decryption is
// the test's own call of OpenSSL (TST-304), the first cipher block is the FIPS-197 C.1 vector
// (TST-305), the packets and sections are built by the helpers below, whose CRC-32 is checked against
// the check value 0x0376E6E7. package_ts is run as a program, with the key on standard input.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "test_support.hpp"

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

const std::string kPackageTs = M15_PACKAGE_TS_BIN;
const fs::path kContentDir = fs::path(M15_REPO_DIR) / "content";

// Keys of Section 5.2.2 and the key of FIPS-197, Appendix C.1.
const std::string kK3 = "3A7F1C9E5B2D8046A1E3C5079B4D6F28";
const std::string kKF1 = "3A7F1C9E5B2D8046A1E3C5079B4D6F2";
const std::string kKF2 = "3A7F1C9E5B2D8046A1E3C5079B4D6F2G";
const std::string kFipsKey = "000102030405060708090a0b0c0d0e0f";

// ---------------------------------------------------------------- helpers (test code only)

std::uint32_t crc32(const Bytes& data) {   // CRC-32/MPEG-2, bit by bit
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t byte : data) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool top = ((crc >> 31) & 1u) != static_cast<std::uint32_t>((byte >> bit) & 1);
            crc <<= 1;
            if (top) crc ^= 0x04C11DB7u;
        }
    }
    return crc;
}

Bytes with_crc(Bytes section) {
    const std::uint32_t crc = crc32(section);
    for (int shift = 24; shift >= 0; shift -= 8) section.push_back(static_cast<std::uint8_t>(crc >> shift));
    return section;
}

// PSI section with `body` after the 5 fixed bytes (table_id_extension, version, section numbers).
Bytes section(std::uint8_t table_id, std::uint16_t extension, const Bytes& body) {
    const std::size_t length = 5 + body.size() + 4;
    Bytes s = {table_id, static_cast<std::uint8_t>(0xB0 | (length >> 8)), static_cast<std::uint8_t>(length & 0xFF),
               static_cast<std::uint8_t>(extension >> 8), static_cast<std::uint8_t>(extension & 0xFF), 0xC1, 0x00, 0x00};
    s.insert(s.end(), body.begin(), body.end());
    return with_crc(s);
}

Bytes pat(const std::vector<std::pair<std::uint16_t, std::uint16_t>>& programs) {   // (number, PMT PID)
    Bytes body;
    for (const auto& [number, pid] : programs) {
        body.insert(body.end(), {static_cast<std::uint8_t>(number >> 8), static_cast<std::uint8_t>(number & 0xFF),
                                 static_cast<std::uint8_t>(0xE0 | (pid >> 8)), static_cast<std::uint8_t>(pid & 0xFF)});
    }
    return section(0x00, 1, body);
}

Bytes pmt(std::uint16_t pcr_pid, const std::vector<std::pair<std::uint8_t, std::uint16_t>>& streams) {   // (type, PID)
    Bytes body = {static_cast<std::uint8_t>(0xE0 | (pcr_pid >> 8)), static_cast<std::uint8_t>(pcr_pid & 0xFF), 0xF0, 0x00};
    for (const auto& [type, pid] : streams) {
        body.insert(body.end(), {type, static_cast<std::uint8_t>(0xE0 | (pid >> 8)), static_cast<std::uint8_t>(pid & 0xFF),
                                 0xF0, 0x00});
    }
    return section(0x02, 1, body);
}

// A 188-byte TS packet. `af_length` < 0: no adaptation field ('01'); otherwise an adaptation field of
// that length ('11', or '10' with `payload_flag` false). The payload is `payload`, then 0xAB filler.
Bytes packet(std::uint16_t pid, bool pusi, int af_length, const Bytes& payload = {}, bool payload_flag = true) {
    Bytes p(188, 0xAB);
    p[0] = 0x47;
    p[1] = static_cast<std::uint8_t>((pusi ? 0x40 : 0x00) | (pid >> 8));
    p[2] = static_cast<std::uint8_t>(pid & 0xFF);
    std::size_t offset = 4;
    if (af_length < 0) {
        p[3] = 0x10;
    } else {
        p[3] = payload_flag ? 0x30 : 0x20;
        p[4] = static_cast<std::uint8_t>(af_length);
        if (af_length > 0) p[5] = 0x00;
        for (int i = 1; i < af_length; ++i) p[5 + i] = 0xFF;
        offset = 5 + static_cast<std::size_t>(af_length);
    }
    for (std::size_t i = 0; i < payload.size() && offset + i < 188; ++i) p[offset + i] = payload[i];
    return p;
}

// A PSI packet: pointer_field 0, the section, 0xFF stuffing.
Bytes psi_packet(std::uint16_t pid, const Bytes& sec) {
    Bytes payload = {0x00};
    payload.insert(payload.end(), sec.begin(), sec.end());
    payload.resize(184, 0xFF);
    return packet(pid, true, -1, payload);
}

void write_file(const std::string& path, const std::vector<Bytes>& packets) {
    std::ofstream out(path, std::ios::binary);
    for (const Bytes& p : packets) out.write(reinterpret_cast<const char*>(p.data()), static_cast<std::streamsize>(p.size()));
}

Bytes read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

Bytes from_hex(const std::string& hex) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

// Independent decryption of `n` bytes: AES-128-CBC, IV = 16 zero bytes, no padding (D-04).
Bytes decrypt(const Bytes& key, const std::uint8_t* data, std::size_t n) {
    std::unique_ptr<EVP_CIPHER_CTX, void (*)(EVP_CIPHER_CTX*)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    const std::uint8_t iv[16] = {};
    Bytes out(n + 16);
    int len = 0;
    int final_len = 0;
    EXPECT_EQ(EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key.data(), iv), 1);
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    EXPECT_EQ(EVP_DecryptUpdate(ctx.get(), out.data(), &len, data, static_cast<int>(n)), 1);
    EXPECT_EQ(EVP_DecryptFinal_ex(ctx.get(), out.data() + len, &final_len), 1);
    out.resize(static_cast<std::size_t>(len + final_len));
    return out;
}

// Payload offset by ISO/IEC 13818-1, 2.4.3.2 / 2.4.3.4: 4 for '01', 5 + adaptation_field_length for
// '11', none for '10' and '00'.
std::size_t payload_offset(const std::uint8_t* p) {
    const int afc = (p[3] >> 4) & 3;
    if (afc == 1) return 4;
    if (afc == 3 && 5 + p[4] < 188) return 5 + p[4];
    return 0;
}

struct RunResult {
    int exit_code = -1;
    std::string output;   // standard output and standard error
};

// Runs package_ts with `key_line` on standard input (a pipe).
RunResult run_package_ts(const std::string& in, const std::string& out, const std::string& key_line, const test::TempDir& dir) {
    const std::string log = dir.file("package_ts.log");
    const std::string command = "'" + kPackageTs + "' --in '" + in + "' --out '" + out + "' > '" + log + "' 2>&1";
    FILE* pipe = popen(command.c_str(), "w");
    if (pipe == nullptr) return {};
    std::fputs((key_line + "\n").c_str(), pipe);
    const int status = pclose(pipe);
    RunResult r;
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const Bytes text = read_file(log);
    r.output.assign(text.begin(), text.end());
    return r;
}

// Runs package_ts on a pseudo terminal, types `key` after the prompt and returns everything the
// terminal showed (the echo of the typed characters would appear here).
RunResult run_on_terminal(const std::string& in, const std::string& out, const std::string& key) {
    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
    if (pid == 0) {
        execl(kPackageTs.c_str(), kPackageTs.c_str(), "--in", in.c_str(), "--out", out.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    RunResult r;
    if (pid < 0) return r;
    auto read_some = [&](int timeout_ms) {
        pollfd pfd{master, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        char buf[4096];
        const ssize_t n = read(master, buf, sizeof buf);
        if (n <= 0) return false;
        r.output.append(buf, static_cast<std::size_t>(n));
        return true;
    };
    while (r.output.find("Key") == std::string::npos && read_some(5000)) {
    }
    const std::string typed = key + "\n";
    EXPECT_EQ(write(master, typed.data(), typed.size()), static_cast<ssize_t>(typed.size()));
    while (read_some(60000)) {
    }
    close(master);
    int status = 0;
    waitpid(pid, &status, 0);
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

// Checks D-04 for one input / output packet pair with the A/V PIDs `av` (TST-304).
::testing::AssertionResult check_pair(const std::uint8_t* clear, const std::uint8_t* enc, const std::vector<std::uint16_t>& av,
                                      const Bytes& key, bool& encrypted) {
    encrypted = false;
    const std::uint16_t pid = static_cast<std::uint16_t>(((clear[1] & 0x1F) << 8) | clear[2]);
    const bool is_av = std::find(av.begin(), av.end(), pid) != av.end();
    const std::size_t offset = payload_offset(clear);
    const bool long_payload = is_av && offset != 0 && 188 - offset >= 16;
    if (!long_payload) {
        if (std::memcmp(clear, enc, 188) != 0) return ::testing::AssertionFailure() << "PID " << pid << ": packet changed";
        return ::testing::AssertionSuccess();
    }
    encrypted = true;
    if (((enc[3] >> 6) & 3) != 2) return ::testing::AssertionFailure() << "PID " << pid << ": scrambling bits not '10'";
    if (std::memcmp(clear, enc, 3) != 0 || (clear[3] & 0x3F) != (enc[3] & 0x3F) ||
        std::memcmp(clear + 4, enc + 4, offset - 4) != 0) {
        return ::testing::AssertionFailure() << "PID " << pid << ": header or adaptation field changed";
    }
    const std::size_t n = (188 - offset) / 16 * 16;
    const Bytes plain = decrypt(key, enc + offset, n);
    if (std::memcmp(plain.data(), clear + offset, n) != 0) return ::testing::AssertionFailure() << "PID " << pid << ": decryption differs";
    if (std::memcmp(clear + offset + n, enc + offset + n, 188 - offset - n) != 0) {
        return ::testing::AssertionFailure() << "PID " << pid << ": residual bytes changed";
    }
    return ::testing::AssertionSuccess();
}

std::vector<std::string> files_in(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir)) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

// ---------------------------------------------------------------- TST-304

TEST(Tst304, HelperCrcCheckValue) {
    const std::string text = "123456789";
    EXPECT_EQ(crc32(Bytes(text.begin(), text.end())), 0x0376E6E7u);
}

TEST(Tst304, RoundTripSyntheticPackets) {
    // A/V PIDs 300 (0x1B) and 301 (0x0F), a third stream 0x06 on 302 that must stay clear; the PAT
    // starts with program 0 (NIT), which is not the program.
    test::TempDir dir;
    const std::vector<Bytes> input = {
        psi_packet(0, pat({{0, 16}, {1, 4096}})),
        psi_packet(4096, pmt(300, {{0x1B, 300}, {0x0F, 301}, {0x06, 302}})),
        packet(300, true, -1, {0x00, 0x00, 0x01, 0xE0}),   // '01', 184 payload bytes: 176 encrypted, 8 clear
        packet(300, false, 7),                            // '11', 176 payload bytes: all encrypted
        packet(300, false, 10),                           // '11', 173 payload bytes: 160 encrypted, 13 clear
        packet(300, false, 167),                          // '11', 16 payload bytes: encrypted
        packet(300, false, 168),                          // '11', 15 payload bytes: unchanged
        packet(300, false, 183, {}, false),               // '10', adaptation field only: unchanged
        packet(301, true, -1, {0xFF, 0xF1}),              // audio, 184 payload bytes
        packet(302, true, -1),                            // other stream type: unchanged
        packet(0x1FFF, false, -1),                        // null packet: unchanged
        psi_packet(0, pat({{0, 16}, {1, 4096}})),         // PSI repetitions: unchanged
    };
    write_file(dir.file("in.ts"), input);
    const RunResult r = run_package_ts(dir.file("in.ts"), dir.file("out.ts"), kK3, dir);
    ASSERT_EQ(r.exit_code, 0) << r.output;
    const Bytes in = read_file(dir.file("in.ts"));
    const Bytes out = read_file(dir.file("out.ts"));
    ASSERT_EQ(out.size(), in.size());   // REQ-63: exactly the input size
    const std::vector<bool> expect_encrypted = {false, false, true, true, true, true, false, false, true, false, false, false};
    for (std::size_t i = 0; i < input.size(); ++i) {
        bool encrypted = false;
        EXPECT_TRUE(check_pair(&in[188 * i], &out[188 * i], {300, 301}, from_hex(kK3), encrypted)) << "packet " << i;
        EXPECT_EQ(encrypted, expect_encrypted[i]) << "packet " << i;
        EXPECT_EQ(((out[188 * i + 3] >> 6) & 3) == 2, expect_encrypted[i]) << "packet " << i;
    }
    EXPECT_EQ(files_in(dir.path()), (std::vector<std::string>{"in.ts", "out.ts", "package_ts.log"}));
}

TEST(Tst304, ContentSvc3WithK3) {
    const fs::path clear = kContentDir / "svc-3.ts";
    ASSERT_TRUE(fs::exists(clear)) << "content missing: run content/generate.sh (" << clear.string() << ")";
    test::TempDir dir;
    const RunResult r = run_package_ts(clear.string(), dir.file("svc-3.enc.ts"), kK3, dir);
    ASSERT_EQ(r.exit_code, 0) << r.output;
    ASSERT_EQ(fs::file_size(dir.file("svc-3.enc.ts")), fs::file_size(clear));   // same size (REQ-63)

    // Video and audio PIDs of the reference set (Section 5.2.1): 256 and 257.
    std::ifstream a(clear, std::ios::binary);
    std::ifstream b(dir.file("svc-3.enc.ts"), std::ios::binary);
    std::uint8_t pa[188];
    std::uint8_t pb[188];
    const Bytes key = from_hex(kK3);
    std::uint64_t packets = 0;
    std::uint64_t encrypted_count = 0;
    std::uint64_t failures = 0;
    while (a.read(reinterpret_cast<char*>(pa), 188) && b.read(reinterpret_cast<char*>(pb), 188)) {
        bool encrypted = false;
        const ::testing::AssertionResult ok = check_pair(pa, pb, {256, 257}, key, encrypted);
        if (!ok && ++failures <= 5) ADD_FAILURE() << "packet " << packets << ": " << ok.message();
        encrypted_count += encrypted ? 1 : 0;
        ++packets;
    }
    EXPECT_EQ(failures, 0u);
    EXPECT_EQ(packets * 188, fs::file_size(clear));
    EXPECT_GT(encrypted_count, packets / 2);   // most packets of a 3.5 Mbit/s A/V stream are encrypted
}

// ---------------------------------------------------------------- TST-305

TEST(Tst305, FirstBlockIsTheFips197Vector) {
    test::TempDir dir;
    // Payload = the FIPS-197 C.1 plaintext followed by 168 bytes (0x00, 0x01, …).
    Bytes payload = from_hex("00112233445566778899aabbccddeeff");
    for (int i = 0; i < 168; ++i) payload.push_back(static_cast<std::uint8_t>(i));
    write_file(dir.file("in.ts"), {psi_packet(0, pat({{1, 4096}})), psi_packet(4096, pmt(256, {{0x1B, 256}, {0x0F, 257}})),
                                   packet(256, true, -1, payload)});
    const RunResult r = run_package_ts(dir.file("in.ts"), dir.file("out.ts"), kFipsKey, dir);
    ASSERT_EQ(r.exit_code, 0) << r.output;
    const Bytes out = read_file(dir.file("out.ts"));
    ASSERT_EQ(out.size(), 3u * 188);
    const std::uint8_t* p = &out[2 * 188];
    EXPECT_EQ(Bytes(p + 4, p + 4 + 16), from_hex("69c4e0d86a7b0430d8cdb78070b4c55a"));   // FIPS-197 C.1 ciphertext
    EXPECT_EQ(Bytes(p + 4 + 176, p + 188), Bytes(payload.begin() + 176, payload.end()));  // residual 8 bytes unchanged
    EXPECT_EQ((p[3] >> 6) & 3, 2);
}

// ---------------------------------------------------------------- TST-306

TEST(Tst306, InvalidInputsGiveExitCode1AndNoOutput) {
    struct Case {
        const char* name;
        std::vector<Bytes> packets;   // empty: no input file
        std::string key;
    };
    const Bytes good_pat = psi_packet(0, pat({{1, 4096}}));
    const Bytes good_pmt = psi_packet(4096, pmt(256, {{0x1B, 256}, {0x0F, 257}}));
    Bytes no_sync = packet(256, true, -1);
    no_sync[0] = 0x46;
    const std::vector<Case> cases = {
        {"missing input file", {}, kK3},
        {"packet without sync byte", {good_pat, good_pmt, packet(256, true, -1), no_sync}, kK3},
        {"input without PAT", {good_pmt, packet(256, true, -1), packet(257, true, -1)}, kK3},
        {"key KF1 (31 characters)", {good_pat, good_pmt, packet(256, true, -1)}, kKF1},
        {"key KF2 (non-hexadecimal character)", {good_pat, good_pmt, packet(256, true, -1)}, kKF2},
    };
    for (const Case& c : cases) {
        test::TempDir dir;
        if (!c.packets.empty()) write_file(dir.file("in.ts"), c.packets);
        const RunResult r = run_package_ts(dir.file("in.ts"), dir.file("out.ts"), c.key, dir);
        EXPECT_EQ(r.exit_code, 1) << c.name;
        EXPECT_NE(r.output.find("ERROR [SRV-PKG]"), std::string::npos) << c.name << ": " << r.output;
        EXPECT_FALSE(fs::exists(dir.file("out.ts"))) << c.name;
        std::vector<std::string> expected = {"package_ts.log"};
        if (!c.packets.empty()) expected.insert(expected.begin(), "in.ts");
        EXPECT_EQ(files_in(dir.path()), expected) << c.name << ": no temporary file left";
        EXPECT_EQ(r.output.find(c.key), std::string::npos) << c.name << ": key in the output";
    }
}

TEST(Tst306, KeyNotEchoedAndOnlyFourCharactersLogged) {
    test::TempDir dir;
    write_file(dir.file("in.ts"), {psi_packet(0, pat({{1, 4096}})), psi_packet(4096, pmt(256, {{0x1B, 256}, {0x0F, 257}})),
                                   packet(256, true, -1), packet(257, true, -1)});
    const RunResult r = run_on_terminal(dir.file("in.ts"), dir.file("out.ts"), kK3);
    ASSERT_EQ(r.exit_code, 0) << r.output;
    EXPECT_NE(r.output.find("Key"), std::string::npos) << r.output;   // the prompt was shown
    // Nothing of the typed key beyond its first 4 characters appears on the terminal or in the log.
    EXPECT_EQ(r.output.find(kK3.substr(0, 5)), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("key=3A7F\xE2\x80\xA6"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("pid=256 encrypted=1 clear=0"), std::string::npos) << r.output;
    EXPECT_TRUE(fs::exists(dir.file("out.ts")));

    // The same with the key on a pipe (standard output, standard error): no full key, K3 in lower case.
    const std::string lower = "3a7f1c9e5b2d8046a1e3c5079b4d6f28";
    const RunResult p = run_package_ts(dir.file("in.ts"), dir.file("out2.ts"), lower, dir);
    ASSERT_EQ(p.exit_code, 0) << p.output;
    EXPECT_EQ(p.output.find(lower), std::string::npos);
    EXPECT_EQ(p.output.find(kK3), std::string::npos);
    EXPECT_NE(p.output.find("key=3A7F\xE2\x80\xA6"), std::string::npos) << p.output;
}

}  // namespace
