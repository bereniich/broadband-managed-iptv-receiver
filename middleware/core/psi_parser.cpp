#include "psi_parser.hpp"

#include <cstdio>
#include <utility>

namespace al {

namespace {

constexpr const char* kModule = "AL-PSI";
constexpr std::uint16_t kPatPid = 0;
constexpr int kPatTableId = 0x00;
constexpr int kPmtTableId = 0x02;

std::string hex2(std::uint8_t value) {
    char text[8];
    std::snprintf(text, sizeof text, "0x%02X", static_cast<unsigned>(value));
    return text;
}

std::string es_text(const std::optional<EsInfo>& es) {
    return es ? std::to_string(es->pid) + "/" + hex2(es->stream_type) : "none";
}

const char* table_name(bool pat) { return pat ? "PAT" : "PMT"; }

}  // namespace

PsiParser::PsiParser(pal::IDemux& demux, pal::ILogger& logger, TaskQueue& queue)
    : demux_(demux), logger_(logger), queue_(queue) {}

PsiParser::~PsiParser() { close(); }

bool PsiParser::start_pat(std::uint64_t session, std::uint64_t generation, Events events) {
    close();
    session_ = session;
    generation_ = generation;
    events_ = std::move(events);
    active_ = true;
    pat_filter_ = open_section_filter(Table::Pat, kPatPid);
    if (pat_filter_ == 0) {
        close();
        return false;
    }
    return true;
}

bool PsiParser::start_pmt(std::uint16_t pmt_pid) {
    if (!active_ || pmt_filter_ != 0) return false;
    pmt_pid_ = pmt_pid;
    pmt_filter_ = open_section_filter(Table::Pmt, pmt_pid);
    return pmt_filter_ != 0;
}

void PsiParser::close() {
    if (pat_filter_ != 0) demux_.close_filter(pat_filter_);
    if (pmt_filter_ != 0) demux_.close_filter(pmt_filter_);
    pat_filter_ = 0;
    pmt_filter_ = 0;
    active_ = false;   // queued sections of the attempt are discarded in on_section()
    events_ = Events{};
    pmt_pid_ = 0;
    pat_version_.reset();
    pmt_version_.reset();
    pat_reported_ = false;
    pmt_reported_ = false;
}

pal::IDemux::FilterId PsiParser::open_section_filter(Table table, std::uint16_t pid) {
    const bool pat = table == Table::Pat;
    const std::uint64_t generation = generation_;
    TaskQueue& queue = queue_;
    // Runs on the tuner thread: copy and post only (rules T1 and 3 of Section 3.9.1).
    auto on_data = [this, &queue, generation, table](const pal::IDemux::FilterData& data) {
        std::vector<std::uint8_t> section(data.data, data.data + data.len);
        queue.post(TaskKind::Data, [this, generation, table, section = std::move(section)] {
            on_section(generation, table, section);
        });
    };
    const pal::FilterConfig config{pal::FilterType::Section, pid, pat ? kPatTableId : kPmtTableId};
    const pal::IDemux::FilterId id = demux_.open_filter(config, on_data);
    if (id == 0) {
        log(pal::LogLevel::Error,
            std::string("cannot open the ") + table_name(pat) + " section filter on PID " + std::to_string(pid));
        return 0;
    }
    if (!demux_.start_filter(id)) {
        demux_.close_filter(id);
        log(pal::LogLevel::Error,
            std::string("cannot start the ") + table_name(pat) + " section filter on PID " + std::to_string(pid));
        return 0;
    }
    return id;
}

void PsiParser::on_section(std::uint64_t generation, Table table, const std::vector<std::uint8_t>& section) {
    if (!active_ || generation != generation_) return;   // released or older attempt
    if (table == Table::Pat) {
        handle_pat(section);
    } else {
        handle_pmt(section);
    }
}

void PsiParser::handle_pat(const std::vector<std::uint8_t>& section) {
    const PatResult pat = parse_pat(section.data(), section.size());
    if (pat.error != PsiError::None) {
        ++discarded_sections_;
        log(pal::LogLevel::Warn, std::string("discarded PAT section: ") + to_string(pat.error));
        return;
    }
    if (pat_version_ && *pat_version_ != pat.version) {
        log(pal::LogLevel::Warn, "PAT version changed " + std::to_string(*pat_version_) + " -> " +
                                     std::to_string(pat.version) + ", ignored");
    }
    pat_version_ = pat.version;
    if (pat_reported_) return;   // repetitions are not parsed further (Section 4.2.4)
    pat_reported_ = true;
    log(pal::LogLevel::Info,
        "PAT program=" + std::to_string(pat.program_number) + " pmt_pid=" + std::to_string(pat.pmt_pid));
    const auto on_pat = events_.on_pat;   // a copy: the handler may call close()
    if (on_pat) on_pat(pat.pmt_pid);
}

void PsiParser::handle_pmt(const std::vector<std::uint8_t>& section) {
    const PmtResult pmt = parse_pmt(section.data(), section.size());
    if (pmt.error != PsiError::None) {
        ++discarded_sections_;
        log(pal::LogLevel::Warn, std::string("discarded PMT section: ") + to_string(pmt.error));
        return;
    }
    if (pmt_version_ && *pmt_version_ != pmt.version) {
        log(pal::LogLevel::Warn, "PMT version changed " + std::to_string(*pmt_version_) + " -> " +
                                     std::to_string(pmt.version) + ", ignored");
    }
    pmt_version_ = pmt.version;
    if (pmt_reported_) return;   // repetitions are not parsed further (Section 4.2.4)
    pmt_reported_ = true;
    // REQ-18: program number and PMT PID are in the PAT line, the other PIDs here.
    log(pal::LogLevel::Info, "PMT pcr_pid=" + std::to_string(pmt.pcr_pid) + " video=" + es_text(pmt.video) +
                                 " audio=" + es_text(pmt.audio));
    if (!pmt.video) {
        const auto on_novideo = events_.on_pmt_novideo;   // a copy: the handler may call close()
        if (on_novideo) on_novideo();
        return;
    }
    StreamDescription description;
    description.program_number = pmt.program_number;
    description.pmt_pid = pmt_pid_;
    description.pcr_pid = pmt.pcr_pid;
    description.video_pid = pmt.video->pid;
    description.video_stream_type = pmt.video->stream_type;
    if (pmt.audio) {
        description.audio_pid = pmt.audio->pid;
        description.audio_stream_type = pmt.audio->stream_type;
    }
    const auto on_pmt = events_.on_pmt;   // a copy: the handler may call close()
    if (on_pmt) on_pmt(description);
}

void PsiParser::log(pal::LogLevel level, const std::string& message) {
    logger_.log(level, kModule,
                message + " session=" + std::to_string(session_) + " generation=" + std::to_string(generation_));
}

}  // namespace al
