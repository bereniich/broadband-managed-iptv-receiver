// AL-PSI: PAT / PMT acquisition through the DTV SDK (Sections 2.2.4, 4.2.4 and 4.6.2; REQ-16 – REQ-18).
// Opens the section filters when AL-SES asks (S3 / T-03 for the PAT, S7 / T-11 for the PMT), checks
// and parses the sections on the AL worker and reports the first valid PAT and PMT of the attempt.
// The filter callbacks only copy the section and post a data task with the generation (rule T1).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pal/platform.hpp"
#include "psi.hpp"
#include "task_queue.hpp"

namespace al {

// Stream description of Section 4.2.3; audio is absent for a stream without audio (REQ-17).
struct StreamDescription {
    std::uint16_t program_number = 0;
    std::uint16_t pmt_pid = 0;
    std::uint16_t pcr_pid = 0;
    std::uint16_t video_pid = 0;
    std::uint8_t video_stream_type = 0;
    std::optional<std::uint16_t> audio_pid;
    std::optional<std::uint8_t> audio_stream_type;
};

class PsiParser {
public:
    // Reported on the AL worker, at most once per attempt each.
    struct Events {
        std::function<void(std::uint16_t pmt_pid)> on_pat;          // EV-PAT
        std::function<void(const StreamDescription&)> on_pmt;       // EV-PMT
        std::function<void()> on_pmt_novideo;                       // EV-PMT-NOVIDEO
    };

    PsiParser(pal::IDemux& demux, pal::ILogger& logger, TaskQueue& queue);
    ~PsiParser();

    PsiParser(const PsiParser&) = delete;
    PsiParser& operator=(const PsiParser&) = delete;

    // S3: opens and starts the PAT section filter (PID 0, table_id 0x00) for the attempt `generation`
    // of session `session`; a previous attempt is closed first. False if the filter cannot be opened
    // or started (logged as an error; nothing is left open).
    bool start_pat(std::uint64_t session, std::uint64_t generation, Events events);

    // S7: opens and starts the PMT section filter (table_id 0x02). False if it cannot be opened or
    // started, or if no attempt is running or its PMT filter is already open.
    bool start_pmt(std::uint16_t pmt_pid);
    // Sections discarded because of a CRC, table_id or length error, since the process started
    // (statistics of Section 4.8.1).
    std::uint64_t discarded_sections() const { return discarded_sections_; }

    // RELEASE: closes the section filters of the attempt; its queued sections are discarded.
    void close();

private:
    enum class Table { Pat, Pmt };

    pal::IDemux::FilterId open_section_filter(Table table, std::uint16_t pid);
    void on_section(std::uint64_t generation, Table table, const std::vector<std::uint8_t>& section);
    void handle_pat(const std::vector<std::uint8_t>& section);
    void handle_pmt(const std::vector<std::uint8_t>& section);
    void log(pal::LogLevel level, const std::string& message);

    pal::IDemux& demux_;
    pal::ILogger& logger_;
    TaskQueue& queue_;

    bool active_ = false;
    std::uint64_t session_ = 0;
    std::uint64_t generation_ = 0;
    Events events_;
    std::uint64_t discarded_sections_ = 0;
    pal::IDemux::FilterId pat_filter_ = 0;
    pal::IDemux::FilterId pmt_filter_ = 0;
    std::uint16_t pmt_pid_ = 0;
    std::optional<std::uint8_t> pat_version_;   // version of the last valid PAT / PMT seen
    std::optional<std::uint8_t> pmt_version_;
    bool pat_reported_ = false;
    bool pmt_reported_ = false;
};

}  // namespace al
