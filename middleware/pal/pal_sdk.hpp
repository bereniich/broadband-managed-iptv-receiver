// PAL-SDK: the DTV SDK of IF-INT-04 (ITuner, IDemux; Section 3.9.2) over the software HAL.
//
// The only PAL module that calls the HAL (rule R4). Lifecycle of the DTV SDKs of Lecture 6: tune the
// tuner, open a filter (a handle of the SDK), start it, receive filter events with the data, stop and
// close it. The tuner feeds every received RTP payload into the demux on the tuner receive thread,
// where the demux delivers to the filter callbacks (Sections 3.9.1 rule 1, 4.5.2).
#pragma once

#include <mutex>

#include "demux.hpp"
#include "ip_tuner.hpp"
#include "pal/platform.hpp"

namespace pal {

// Forwards the log lines of the HAL (rule R1: the HAL only calls back) to the PAL logger.
hal::LogFn hal_log_to(ILogger* logger);

class SdkDemux : public IDemux {
public:
    explicit SdkDemux(ILogger* logger);

    // Returns 0 for a PID above 0x1FFF, an unknown type, a section table_id outside -1 … 255 or an
    // empty callback. TS filters ignore table_id.
    FilterId open_filter(const FilterConfig& config, DataCallback on_data) override;
    bool start_filter(FilterId id) override;
    void stop_filter(FilterId id) override;
    void close_filter(FilterId id) override;
    // HAL-DSC (REQ-50): the key is copied into the descrambler of the demux before the call returns
    // (rule 3); the key itself is never logged (REQ-55). Invalid arguments are logged and ignored.
    void set_key(const std::uint16_t* pids, std::size_t pid_count, const std::uint8_t key[16]) override;
    void clear_keys() override;
    Stats stats() const override;

    // Used by SdkTuner on the tuner receive thread and at tune().
    void feed(std::uint8_t* packets, std::size_t count) { demux_.feed(packets, count); }
    void reset_stats() { demux_.reset_stats(); }

private:
    ILogger* logger_;
    hal::Demux demux_;
};

class SdkTuner : public ITuner {
public:
    SdkTuner(SdkDemux& demux, ILogger* logger, hal::IpTuner::Options options = hal::IpTuner::Options{});

    void set_status_handler(StatusCallback on_status) override;
    // Binds 127.0.0.1:<udp_port> (Section 4.2.5); the receive thread is created with SIGINT and
    // SIGTERM blocked. Clears the statistics of tuner and demux (one session, one set of counters).
    bool tune(const TuneParams& params) override;
    void untune() override;
    Stats stats() const override;

    // The port actually bound (tests and tools; 0 when untuned).
    std::uint16_t bound_port() const { return tuner_.bound_port(); }
    // Platform::stop(): untunes and refuses every later tune(), so that no tuner or demux callback
    // follows the stop (Section 3.9.1, rule 4).
    void shutdown();

private:
    void on_lock(bool locked);

    SdkDemux& demux_;
    ILogger* logger_;
    std::mutex handler_mutex_;
    StatusCallback on_status_;
    std::mutex tune_mutex_;  // serializes tune() / shutdown()
    bool shut_down_ = false;
    hal::IpTuner tuner_;  // last member: its thread ends before the members above are destroyed
};

}  // namespace pal
