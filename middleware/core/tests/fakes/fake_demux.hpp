// Fake demux and descrambler of the AL unit tests (Sections 2.2.7 and 5.3.1, "AL fakes"): filters
// fed from byte arrays or from a content file; descrambling by the reference implementation of D-04.
// Behavior follows the contract of IDemux (Section 3.9.2) and REQ-50; callbacks are called on the
// thread that feeds the packets.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "fakes/call_log.hpp"
#include "pal/platform.hpp"

namespace fakes {

class FakeDemux : public pal::IDemux {
public:
    static constexpr std::size_t kMaxSectionSize = 4096;   // B3

    explicit FakeDemux(CallLog* log = nullptr) : log_(log) {}

    FilterId open_filter(const pal::FilterConfig& config, DataCallback on_data) override;
    bool start_filter(FilterId id) override;
    void stop_filter(FilterId id) override;
    void close_filter(FilterId id) override;
    void set_key(const std::uint16_t* pids, std::size_t pid_count, const std::uint8_t key[16]) override;
    void clear_keys() override;
    Stats stats() const override { return stats_; }

    // ---- test side
    // Feeds one 188-byte packet: sync check, continuity check, descrambling, delivery to the
    // started filters of its PID.
    void feed_packet(const std::uint8_t* packet);
    // Feeds a byte stream of whole packets (a trailing partial packet is ignored).
    void feed(const std::vector<std::uint8_t>& ts);
    // Feeds `count` packets from packet number `first` of a file. Returns the number fed; a missing
    // file is reported as a test failure ("content missing: run content/generate.sh").
    std::size_t feed_file(const std::string& path, std::size_t first, std::size_t count);

    struct OpenFilter {
        FilterId id;
        pal::FilterConfig config;
        bool started;
    };
    std::vector<OpenFilter> open_filters() const;
    // open_filter returns 0 while set (error path "filter cannot be created", Section 3.9.2).
    void fail_open_filters(bool fail) { fail_open_ = fail; }
    // open_filter returns 0 from its k-th call after this call on (1 = the next one); 0 = never.
    void fail_open_filters_from(std::size_t k) { fail_open_from_ = k == 0 ? 0 : opens_ + k; }
    bool has_key(std::uint16_t pid) const { return keys_.count(pid) != 0; }

private:
    struct Filter {
        pal::FilterConfig config;
        DataCallback on_data;
        bool started = false;
        bool assembling = false;
        std::vector<std::uint8_t> section;
    };

    void deliver_sections(FilterId id, const std::uint8_t* payload, std::size_t len, bool pusi);
    void extract_sections(FilterId id);

    CallLog* log_;
    FilterId next_id_ = 1;
    std::map<FilterId, Filter> filters_;
    std::map<std::uint16_t, std::array<std::uint8_t, 16>> keys_;
    std::map<std::uint16_t, std::uint8_t> last_cc_;
    Stats stats_{0, 0, 0};
    bool fail_open_ = false;
    std::size_t opens_ = 0;            // calls of open_filter
    std::size_t fail_open_from_ = 0;   // see fail_open_filters_from
};

}  // namespace fakes
