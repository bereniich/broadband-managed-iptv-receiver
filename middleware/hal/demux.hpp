// HAL-DMX: software demultiplexer (REQ-15, REQ-19, Sections 3.9.2 and 4.5).
//
// feed() runs on the tuner receive thread. It checks the sync byte and the continuity counter of
// every TS packet and delivers each packet of a PID to every started filter of that PID: a TS filter
// gets the whole packet, a section filter gets every complete PSI section (table_id … CRC_32, the CRC
// is not checked) assembled in its own buffer of 4 096 bytes (B3). The filter table is protected by
// one mutex that feed() holds while it delivers one packet, so that remove_filter() returning
// guarantees that the filter gets no further callback (Section 3.9.1, rule 4; Section 4.5.2).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "hal_log.hpp"

namespace hal {

class Demux {
public:
    using FilterId = std::uint32_t;  // 0 = invalid
    enum class FilterType { Section, Ts };
    // `data`: a section or one TS packet; valid only during the call. `scrambled`: TS filters only,
    // transport_scrambling_control != '00' (the descrambler of step 7 will clear it after decryption).
    using DataCallback = std::function<void(const std::uint8_t* data, std::size_t len, bool scrambled)>;

    struct Stats {
        std::uint64_t ts_packets = 0;          // packets fed
        std::uint64_t sync_errors = 0;         // dropped: sync byte not 0x47
        std::uint64_t cc_errors = 0;           // continuity counter discontinuities
        std::uint64_t sections_discarded = 0;  // longer than 4 096 bytes, or interrupted (B3, ER-24)
    };

    static constexpr std::size_t kSectionBufferSize = 4096;  // B3

    explicit Demux(LogFn log = nullptr);

    // Creates a stopped filter. Returns 0 for a PID above 0x1FFF, a table_id outside -1 … 255 or
    // an empty callback. `table_id` applies to section filters only (-1 = all).
    FilterId add_filter(FilterType type, std::uint16_t pid, int table_id, DataCallback on_data);
    // Starts / stops delivery; a stopped section filter drops its partial section.
    bool start_filter(FilterId id);
    void stop_filter(FilterId id);
    // Destroys the filter; no callback for it after the return.
    void remove_filter(FilterId id);

    // Feeds `count` TS packets of 188 bytes (one RTP payload). The packets may be changed in place
    // (descrambler, step 7). Callbacks run on the calling thread with the filter table locked. A call
    // of add_filter, start_filter, stop_filter, remove_filter, feed or reset_stats from a callback
    // would deadlock; it is refused instead (logged as an error, 0 / false / no effect). stats() works.
    void feed(std::uint8_t* packets, std::size_t count);

    Stats stats() const;
    // Clears the statistics and the continuity-counter state (a new tuning, a new stream).
    void reset_stats();

private:
    struct Filter {
        FilterId id = 0;
        FilterType type = FilterType::Ts;
        std::uint16_t pid = 0;
        int table_id = -1;
        bool started = false;
        DataCallback on_data;
        // Section assembly (B3).
        std::vector<std::uint8_t> section;
        bool assembling = false;
    };

    void feed_packet(std::uint8_t* p);
    void assemble(Filter& f, const std::uint8_t* payload, std::size_t len, bool unit_start, bool continuous);
    void process_sections(Filter& f);
    void discard_section(Filter& f, const char* reason);
    bool refuse_in_callback(const char* method) const;

    LogFn log_;
    mutable std::mutex mutex_;
    FilterId next_id_ = 1;
    std::map<FilterId, std::unique_ptr<Filter>> filters_;
    std::array<std::vector<Filter*>, 8192> by_pid_{};
    std::array<std::int8_t, 8192> last_cc_{};  // -1 = no packet of the PID yet
    Stats stats_;
    std::chrono::steady_clock::time_point last_discard_log_{};
    std::uint64_t discards_not_logged_ = 0;
};

}  // namespace hal
