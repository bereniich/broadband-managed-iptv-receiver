#include "pal_sdk.hpp"

#include <memory>
#include <string>

#include "pal_signals.hpp"

namespace pal {

namespace {

constexpr const char* kModule = "PAL-SDK";
constexpr std::uint16_t kMaxPid = 0x1FFF;

LogLevel to_pal(hal::LogLevel level) {
    switch (level) {
        case hal::LogLevel::Debug: return LogLevel::Debug;
        case hal::LogLevel::Info: return LogLevel::Info;
        case hal::LogLevel::Warn: return LogLevel::Warn;
        case hal::LogLevel::Error: return LogLevel::Error;
    }
    return LogLevel::Info;
}

}  // namespace

hal::LogFn hal_log_to(ILogger* logger) {
    if (logger == nullptr) return nullptr;
    return [logger](hal::LogLevel level, const char* module, const std::string& message) {
        logger->log(to_pal(level), module, message);
    };
}

// ------------------------------------------------------------------------------------------- demux

SdkDemux::SdkDemux(ILogger* logger) : logger_(logger), demux_(hal_log_to(logger)) {}

IDemux::FilterId SdkDemux::open_filter(const FilterConfig& config, DataCallback on_data) {
    if (config.pid > kMaxPid || !on_data) return 0;
    hal::Demux::FilterType type;
    int table_id = -1;
    switch (config.type) {
        case FilterType::Section:
            type = hal::Demux::FilterType::Section;
            table_id = config.table_id;
            break;
        case FilterType::Ts:
            type = hal::Demux::FilterType::Ts;
            break;
        default:
            return 0;
    }
    // The handle of the SDK is the id of the HAL filter. The callback needs it, so it reads it from a
    // slot that is filled before the filter can be started (filters are created stopped).
    auto slot = std::make_shared<FilterId>(0);
    ILogger* logger = logger_;
    auto deliver = [slot, logger, on_data = std::move(on_data)](const std::uint8_t* data, std::size_t len,
                                                                 bool scrambled) {
        const FilterData event{*slot, data, len, scrambled};
        try {
            on_data(event);
        } catch (...) {
            if (logger != nullptr) logger->log(LogLevel::Error, kModule, "a filter callback threw an exception");
        }
    };
    const FilterId id = demux_.add_filter(type, config.pid, table_id, std::move(deliver));
    *slot = id;  // published to the receive thread by the mutex of start_filter() and feed()
    return id;
}

bool SdkDemux::start_filter(FilterId id) { return demux_.start_filter(id); }

void SdkDemux::stop_filter(FilterId id) { demux_.stop_filter(id); }

void SdkDemux::close_filter(FilterId id) { demux_.remove_filter(id); }

void SdkDemux::set_key(const std::uint16_t* pids, std::size_t pid_count, const std::uint8_t key[16]) {
    std::string list;
    for (std::size_t i = 0; pids != nullptr && i < pid_count; ++i) list += (i > 0 ? "," : "") + std::to_string(pids[i]);
    if (!demux_.set_key(pids, pid_count, key)) {
        if (logger_ != nullptr) logger_->log(LogLevel::Error, kModule, "set_key refused for PIDs " + list);
        return;
    }
    if (logger_ != nullptr) logger_->log(LogLevel::Info, kModule, "descrambler key set for PIDs " + list);
}

void SdkDemux::clear_keys() {
    demux_.clear_keys();
    if (logger_ != nullptr) logger_->log(LogLevel::Debug, kModule, "descrambler keys cleared");
}

IDemux::Stats SdkDemux::stats() const {
    const hal::Demux::Stats s = demux_.stats();
    Stats out;
    out.ts_packets = s.ts_packets;
    out.sync_errors = s.sync_errors;
    out.cc_errors = s.cc_errors;
    return out;
}

// ------------------------------------------------------------------------------------------- tuner

SdkTuner::SdkTuner(SdkDemux& demux, ILogger* logger, hal::IpTuner::Options options)
    : demux_(demux),
      logger_(logger),
      tuner_([this](std::uint8_t* ts, std::size_t count) { demux_.feed(ts, count); },
             [this](bool locked) { on_lock(locked); }, hal_log_to(logger), options) {}

void SdkTuner::set_status_handler(StatusCallback on_status) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    on_status_ = std::move(on_status);
}

void SdkTuner::on_lock(bool locked) {
    StatusCallback handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = on_status_;
    }
    if (!handler) return;
    try {
        handler(locked ? TunerStatus::Locked : TunerStatus::Unlocked);
    } catch (...) {
        if (logger_ != nullptr) logger_->log(LogLevel::Error, kModule, "the tuner status callback threw an exception");
    }
}

bool SdkTuner::tune(const TuneParams& params) {
    // From a tuner callback: refused (and logged) by the HAL before anything is changed.
    if (tuner_.on_receive_thread()) return tuner_.tune(params.udp_port, params.ssrc);
    std::lock_guard<std::mutex> lock(tune_mutex_);
    if (shut_down_) {
        if (logger_ != nullptr) logger_->log(LogLevel::Warn, kModule, "tune() after Platform::stop() refused");
        return false;
    }
    BlockStopSignals block;  // the receive thread inherits the blocked SIGINT / SIGTERM
    tuner_.untune();         // no packet of the old tuning reaches the demux after the reset below
    demux_.reset_stats();
    return tuner_.tune(params.udp_port, params.ssrc);
}

void SdkTuner::untune() { tuner_.untune(); }

void SdkTuner::shutdown() {
    std::lock_guard<std::mutex> lock(tune_mutex_);
    shut_down_ = true;
    tuner_.untune();
}

ITuner::Stats SdkTuner::stats() const {
    const hal::IpTuner::Stats s = tuner_.stats();
    Stats out;
    out.rtp_packets = s.rtp_packets;
    out.rtp_lost = s.rtp_lost;
    out.rtp_invalid = s.rtp_invalid;
    return out;
}

}  // namespace pal
