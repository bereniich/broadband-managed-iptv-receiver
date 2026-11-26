// pal_dump: integration test tool of CFG-RXP (Section 5.4.2). It takes the place of the AL above
// the real PAL and HAL:
//   pal_dump --port 5004 --ssrc <n> --seconds <s> [--pids <list>] [--key-stdin] --out <file>
// - creates the platform and tunes (the platform is not started: no UI port, hls_dir untouched);
// - opens section filters on PID 0 and on the PMT PID and prints every PAT and PMT section in
//   hexadecimal (with its CRC-32 check);
// - opens TS filters on the given PIDs, by default the PIDs of the PMT (PAT, PMT and the elementary
//   streams, so that the file can be probed), and writes their packets to <file>. Like an HLS segment
//   (REQ-23) the file starts with the last PAT, the last PMT and the first video key frame (random
//   access point), and ends before the video PES that was still incomplete when the capture stopped,
//   so that the decode checks of TST-511 / TST-512 see only complete pictures; the numbers of the
//   packets left out are printed;
// - with --key-stdin reads a key (32 hexadecimal characters) from standard input and sets it for
//   the audio and video PIDs before their TS filters are opened;
// - at the end prints the tuner and demux statistics and the number of video PES starts whose
//   payload begins with 00 00 01.
// Like the AL, the filter callbacks only copy and hand the work to the main thread.
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "pal/platform.hpp"

namespace {

struct Options {
    int port = -1;
    long long ssrc = -1;
    int seconds = -1;
    std::vector<std::uint16_t> pids;  // empty: the PIDs of the PMT
    bool key_stdin = false;
    std::string out;
};

int usage(const std::string& error) {
    std::fprintf(stderr, "pal_dump: %s\n", error.c_str());
    std::fprintf(stderr,
                 "usage: pal_dump --port <udp port> --ssrc <n> --seconds <s> [--pids <p1,p2,...>] [--key-stdin] "
                 "--out <file>\n");
    return 1;
}

bool parse_number(const std::string& text, long long min, long long max, long long* value) {
    if (text.empty() || text.size() > 12) return false;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    const long long v = std::stoll(text);
    if (v < min || v > max) return false;
    *value = v;
    return true;
}

bool parse_args(int argc, char** argv, Options* o, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        long long v = 0;
        if (arg == "--key-stdin") {
            o->key_stdin = true;
        } else if (!has_value) {
            *error = "missing value for " + arg;
            return false;
        } else if (arg == "--port") {
            if (!parse_number(argv[++i], 1, 65535, &v)) return *error = "invalid --port", false;
            o->port = static_cast<int>(v);
        } else if (arg == "--ssrc") {
            if (!parse_number(argv[++i], 0, 4294967295LL, &v)) return *error = "invalid --ssrc", false;
            o->ssrc = v;
        } else if (arg == "--seconds") {
            if (!parse_number(argv[++i], 1, 86400, &v)) return *error = "invalid --seconds", false;
            o->seconds = static_cast<int>(v);
        } else if (arg == "--pids") {
            std::string list = argv[++i];
            std::size_t pos = 0;
            while (pos <= list.size()) {
                const std::size_t comma = std::min(list.find(',', pos), list.size());
                if (!parse_number(list.substr(pos, comma - pos), 0, 0x1FFF, &v)) return *error = "invalid --pids", false;
                o->pids.push_back(static_cast<std::uint16_t>(v));
                pos = comma + 1;
            }
        } else if (arg == "--out") {
            o->out = argv[++i];
        } else {
            *error = "unknown argument " + arg;
            return false;
        }
    }
    if (o->port < 0 || o->ssrc < 0 || o->seconds < 0 || o->out.empty()) {
        *error = "--port, --ssrc, --seconds and --out are required";
        return false;
    }
    return true;
}

bool parse_key(std::string text, std::uint8_t key[16]) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }), text.end());
    if (text.size() != 32) return false;
    for (std::size_t i = 0; i < 16; ++i) {
        unsigned value = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = text[2 * i + static_cast<std::size_t>(k)];
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
            value = value * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(c) - 'a' + 10));
        }
        key[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}

std::uint32_t crc32_mpeg2(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

std::string hex(const std::vector<std::uint8_t>& data) {
    std::string out;
    char byte[4];
    for (std::uint8_t b : data) {
        std::snprintf(byte, sizeof(byte), "%02x ", b);
        out += byte;
    }
    if (!out.empty()) out.pop_back();
    return out;
}

struct Stream {
    std::uint16_t pid;
    int type;
};

// Work handed from the filter callbacks to the main thread (they never call the SDK themselves).
struct Event {
    char kind;  // 'A' PAT section, 'M' PMT section
    std::vector<std::uint8_t> section;
};

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::string error;
    if (!parse_args(argc, argv, &o, &error)) return usage(error);

    std::uint8_t key[16] = {};
    if (o.key_stdin) {
        std::string line;
        std::getline(std::cin, line);
        if (!parse_key(line, key)) return usage("invalid key on standard input: 32 hexadecimal characters expected");
    }
    std::ofstream out(o.out, std::ios::binary | std::ios::trunc);
    if (!out) return usage("cannot write " + o.out);

    pal::PlatformConfig config;  // the platform is not started: ui_http_port and hls_dir are not used
    config.log_file = "logs/pal_dump.log";
    std::unique_ptr<pal::Platform> platform = pal::create_platform(config);
    if (!platform) return usage("cannot create the platform (log file " + config.log_file + ")");
    pal::IDemux& demux = platform->demux();
    pal::ITuner& tuner = platform->tuner();

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Event> events;
    std::vector<std::uint8_t> pending;  // TS packets not yet written
    std::size_t written_packets = 0;
    bool started = false;                       // the first key frame has been reached
    std::vector<std::uint8_t> last_pat, last_pmt;  // the newest PAT and PMT packets before it
    std::size_t skipped_before = 0;             // packets before the first key frame
    std::size_t bytes_queued = 0;               // bytes handed to the file so far
    std::size_t last_video_start = 0;           // file offset of the newest video PES start
    std::size_t packets_at_last_start = 0;      // written packets before that offset
    int pmt_pid_seen = -1;
    std::set<std::uint16_t> written_pids;
    std::uint16_t video_pid = 0x2000;  // set from the PMT
    long pes_starts = 0, pes_starts_000001 = 0, pes_starts_scrambled = 0;

    auto section_callback = [&](char kind) {
        return [&, kind](const pal::IDemux::FilterData& d) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back({kind, std::vector<std::uint8_t>(d.data, d.data + d.len)});
            cv.notify_one();
        };
    };
    auto queue_packet = [&](const std::uint8_t* data) {  // called with the mutex held
        pending.insert(pending.end(), data, data + pal::kTsPacketSize);
        bytes_queued += pal::kTsPacketSize;
        written_pids.insert(static_cast<std::uint16_t>(((data[1] & 0x1F) << 8) | data[2]));
        ++written_packets;
    };
    auto ts_callback = [&](const pal::IDemux::FilterData& d) {
        std::lock_guard<std::mutex> lock(mutex);
        const std::uint16_t pid = static_cast<std::uint16_t>(((d.data[1] & 0x1F) << 8) | d.data[2]);
        const bool unit_start = (d.data[1] & 0x40) != 0;
        const bool random_access = ((d.data[3] >> 4) & 0x2) != 0 && d.data[4] > 0 && (d.data[5] & 0x40) != 0;
        if (pid == video_pid && unit_start) {
            if (!started && random_access) {
                started = true;
                if (!last_pat.empty()) queue_packet(last_pat.data());
                if (!last_pmt.empty()) queue_packet(last_pmt.data());
            }
            if (started) {
                last_video_start = bytes_queued;
                packets_at_last_start = written_packets;
            }
        }
        if (!started && video_pid != 0x2000) {
            if (pid == 0) last_pat.assign(d.data, d.data + d.len);
            if (pid == pmt_pid_seen) last_pmt.assign(d.data, d.data + d.len);
            ++skipped_before;
        } else {
            if (video_pid == 0x2000) started = true;  // no video PID among the filters: write everything
            queue_packet(d.data);
        }
        // A video PES start: payload_unit_start_indicator = 1 and a payload of at least 16 bytes
        // (shorter payloads are never encrypted, D-04; REQ-44).
        const int afc = (d.data[3] >> 4) & 0x3;
        if (pid == video_pid && (d.data[1] & 0x40) != 0 && (afc & 0x1) != 0) {
            const std::size_t offset = (afc & 0x2) != 0 ? 5u + d.data[4] : 4u;
            if (offset + 16 <= pal::kTsPacketSize) {
                ++pes_starts;
                if (d.scrambled) ++pes_starts_scrambled;
                if (d.data[offset] == 0x00 && d.data[offset + 1] == 0x00 && d.data[offset + 2] == 0x01) ++pes_starts_000001;
            }
        }
    };

    const auto pat_filter = demux.open_filter({pal::FilterType::Section, 0, 0x00}, section_callback('A'));
    demux.start_filter(pat_filter);
    if (!tuner.tune({static_cast<std::uint16_t>(o.port), static_cast<std::uint32_t>(o.ssrc)})) {
        return usage("cannot open UDP port 127.0.0.1:" + std::to_string(o.port));
    }
    std::fprintf(stdout, "tuned 127.0.0.1:%d ssrc=%lld for %d s\n", o.port, o.ssrc, o.seconds);

    int pmt_pid = -1;
    bool ts_filters_open = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(o.seconds);
    std::unique_lock<std::mutex> lock(mutex);
    while (std::chrono::steady_clock::now() < end) {
        cv.wait_until(lock, std::min(end, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)),
                      [&] { return !events.empty(); });
        std::vector<std::uint8_t> to_write;
        to_write.swap(pending);
        std::deque<Event> batch;
        batch.swap(events);
        lock.unlock();
        out.write(reinterpret_cast<const char*>(to_write.data()), static_cast<std::streamsize>(to_write.size()));

        for (const Event& e : batch) {
            const auto& s = e.section;
            const bool crc_ok = s.size() >= 4 && crc32_mpeg2(s.data(), s.size()) == 0;
            std::string info;
            if (e.kind == 'A' && s.size() >= 12) {
                // Program loop: entries of 4 bytes between the 8-byte header and the CRC.
                for (std::size_t i = 8; i + 4 <= s.size() - 4; i += 4) {
                    const int program = (s[i] << 8) | s[i + 1];
                    const int pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
                    if (program == 0) continue;  // network PID
                    info += " program " + std::to_string(program) + " -> PMT PID " + std::to_string(pid);
                    if (pmt_pid < 0 && crc_ok) {
                        pmt_pid = pid;
                        const auto f = demux.open_filter({pal::FilterType::Section, static_cast<std::uint16_t>(pid), 0x02},
                                                         section_callback('M'));
                        demux.start_filter(f);
                    }
                }
                std::fprintf(stdout, "PAT %s (CRC %s)%s\n", hex(s).c_str(), crc_ok ? "ok" : "ERROR", info.c_str());
            } else if (e.kind == 'M' && s.size() >= 16) {
                std::vector<Stream> streams;
                const int pcr_pid = ((s[8] & 0x1F) << 8) | s[9];
                const std::size_t program_info = static_cast<std::size_t>(((s[10] & 0x0F) << 8) | s[11]);
                for (std::size_t i = 12 + program_info; i + 5 <= s.size() - 4;) {
                    const int type = s[i];
                    const std::uint16_t pid = static_cast<std::uint16_t>(((s[i + 1] & 0x1F) << 8) | s[i + 2]);
                    const std::size_t es_info = static_cast<std::size_t>(((s[i + 3] & 0x0F) << 8) | s[i + 4]);
                    streams.push_back({pid, type});
                    i += 5 + es_info;
                }
                info = " PCR PID " + std::to_string(pcr_pid) + ", streams";
                for (const Stream& st : streams) {
                    char text[32];
                    std::snprintf(text, sizeof(text), " %u/0x%02X", st.pid, st.type);
                    info += text;
                }
                std::fprintf(stdout, "PMT %s (CRC %s)%s\n", hex(s).c_str(), crc_ok ? "ok" : "ERROR", info.c_str());
                if (!ts_filters_open && crc_ok) {
                    ts_filters_open = true;
                    std::vector<std::uint16_t> av;
                    std::vector<std::uint16_t> pids = o.pids;
                    if (pids.empty()) {
                        pids = {0, static_cast<std::uint16_t>(pmt_pid)};
                        for (const Stream& st : streams) pids.push_back(st.pid);
                    }
                    for (const Stream& st : streams) {
                        const bool filtered = std::find(pids.begin(), pids.end(), st.pid) != pids.end();
                        if (st.type == 0x1B && video_pid == 0x2000 && filtered) {
                            std::lock_guard<std::mutex> guard(mutex);
                            video_pid = st.pid;
                            pmt_pid_seen = pmt_pid;
                        }
                        if (st.type == 0x1B || st.type == 0x0F) av.push_back(st.pid);
                    }
                    if (o.key_stdin) {  // the key before the TS filters, as AL-DSC does (REQ-49)
                        demux.set_key(av.data(), av.size(), key);
                        std::string pids;
                        for (std::uint16_t p : av) pids += (pids.empty() ? "" : ",") + std::to_string(p);
                        char shown[8];
                        std::snprintf(shown, sizeof(shown), "%02X%02X", key[0], key[1]);
                        std::fprintf(stdout, "key %s… set for PIDs %s\n", shown, pids.c_str());  // REQ-55: 4 characters
                    }
                    for (std::uint16_t p : pids) {
                        const auto f = demux.open_filter({pal::FilterType::Ts, p}, ts_callback);
                        demux.start_filter(f);
                    }
                }
            }
        }
        std::fflush(stdout);
        lock.lock();
    }
    lock.unlock();

    tuner.untune();  // no callback after this
    std::vector<std::uint8_t> rest;
    {
        std::lock_guard<std::mutex> guard(mutex);
        rest.swap(pending);
    }
    out.write(reinterpret_cast<const char*>(rest.data()), static_cast<std::streamsize>(rest.size()));
    out.close();
    std::memset(key, 0, sizeof(key));
    // The newest video PES was cut by the end of the capture: the file ends before it.
    std::size_t dropped_after = 0;
    if (video_pid != 0x2000 && last_video_start > 0 && last_video_start < bytes_queued) {
        std::error_code ec;
        std::filesystem::resize_file(o.out, last_video_start, ec);
        if (!ec) {
            dropped_after = written_packets - packets_at_last_start;
            written_packets = packets_at_last_start;
        }
    }

    const auto t = tuner.stats();
    const auto d = demux.stats();
    std::string pids;
    for (std::uint16_t p : written_pids) pids += (pids.empty() ? "" : ",") + std::to_string(p);
    std::fprintf(stdout, "tuner rtp_packets=%llu rtp_lost=%llu rtp_invalid=%llu\n",
                 static_cast<unsigned long long>(t.rtp_packets), static_cast<unsigned long long>(t.rtp_lost),
                 static_cast<unsigned long long>(t.rtp_invalid));
    std::fprintf(stdout, "demux ts_packets=%llu sync_errors=%llu cc_errors=%llu\n",
                 static_cast<unsigned long long>(d.ts_packets), static_cast<unsigned long long>(d.sync_errors),
                 static_cast<unsigned long long>(d.cc_errors));
    std::fprintf(stdout, "video PES starts=%ld (received scrambled: %ld), beginning with 00 00 01: %ld\n", pes_starts,
                 pes_starts_scrambled, pes_starts_000001);
    std::fprintf(stdout, "written %s: %zu packets, PIDs %s (left out: %zu before the first key frame, %zu of the incomplete last picture)\n",
                 o.out.c_str(), written_packets, pids.c_str(), skipped_before, dropped_after);
    if (!out) {
        std::fprintf(stderr, "pal_dump: error writing %s\n", o.out.c_str());
        return 1;
    }
    return 0;
}
