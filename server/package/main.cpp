// SRV-PKG: build/bin/package_ts --in <clear.ts> --out <enc.ts> (Section 4.7.3, REQ-63).
// The key (32 hexadecimal characters) is read from standard input; on a terminal the echo is switched
// off, so that the key appears neither on the screen nor in the shell history or the process list.
// Log lines (NFR-13) go to standard error and show at most the first 4 key characters (REQ-55).
// Exit code 0 on success, 1 on every error of ER-39 (no output file is written then).
#include <cctype>
#include <iostream>
#include <optional>
#include <string>

#include <termios.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "log.hpp"
#include "packager.hpp"

namespace {

constexpr const char* kModule = "SRV-PKG";

const char* const kUsage = "usage: package_ts --in <clear.ts> --out <enc.ts>  (key on standard input)";

// Reads one line from standard input; on a terminal with the echo switched off.
bool read_key_line(std::string& line) {
    const bool terminal = isatty(STDIN_FILENO) == 1;
    termios saved{};
    if (terminal) {
        // The echo is off before the prompt appears, so that no typed character can be shown.
        if (tcgetattr(STDIN_FILENO, &saved) == 0) {
            termios silent = saved;
            silent.c_lflag &= static_cast<tcflag_t>(~ECHO);
            tcsetattr(STDIN_FILENO, TCSANOW, &silent);
        }
        std::cerr << "Key (32 hexadecimal characters): " << std::flush;
    }
    const bool ok = static_cast<bool>(std::getline(std::cin, line));
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        std::cerr << '\n';
    }
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::string in;
    std::string out;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        if (option == "--in") {
            in = argv[i + 1];
        } else if (option == "--out") {
            out = argv[i + 1];
        } else {
            in.clear();
            break;
        }
    }
    if (argc != 5 || in.empty() || out.empty()) {
        std::cerr << kUsage << '\n';
        return 1;
    }

    srv::Logger log("", &std::cerr);
    std::string text;
    if (!read_key_line(text)) {
        log.error(kModule, "no key on standard input");
        return 1;
    }
    std::optional<pkg::Key> key = pkg::parse_key(text);
    const std::string shown = pkg::masked_key(text);
    OPENSSL_cleanse(text.data(), text.size());
    if (!key) {
        log.error(kModule, "the key must be exactly 32 hexadecimal characters");
        return 1;
    }
    log.info(kModule, "key=" + shown);

    const pkg::PackageResult result = pkg::package_file(in, out, *key);
    OPENSSL_cleanse(key->data(), key->size());
    if (!result.ok) {
        log.error(kModule, result.error + "; no output file written");
        return 1;
    }
    std::string av;
    for (const std::uint16_t pid : result.av_pids) av += (av.empty() ? "" : ",") + std::to_string(pid);
    log.info(kModule, "pmt_pid=" + std::to_string(result.pmt_pid) + " av_pids=" + av);
    for (const auto& [pid, count] : result.counts) {
        log.info(kModule, "pid=" + std::to_string(pid) + " encrypted=" + std::to_string(count.encrypted) +
                              " clear=" + std::to_string(count.clear));
    }
    log.info(kModule, "wrote " + out + " (" + std::to_string(result.bytes) + " bytes, the size of " + in + ")");
    return 0;
}
