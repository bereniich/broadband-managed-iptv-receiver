// AL-DSC: key manager and validator (Sections 2.2.4, 4.1.3, 4.2.3 and 4.2.4; REQ-44, REQ-49, REQ-51,
// REQ-53 – REQ-55). Keeps the keys per service, sets the key of the service on the descrambler before
// the TS filters are started (S8), opens the TS filters of the output (REQ-19), decides protection on
// the first video packet with at least 16 payload bytes (S9) and checks the first three PES starts
// received with scrambled = true (S10). State changes and discarding a rejected key are done by AL-SES
// (rule 3 of Section 4.3.1); AL-DSC only reports the events.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "key_store.hpp"
#include "pal/platform.hpp"
#include "psi_parser.hpp"
#include "task_queue.hpp"
#include "ts_packet.hpp"

namespace al {

enum class Protection {
    Clear,            // scrambled = false
    ProtectedNoKey,   // scrambled = true, no key was set at T-12 of this attempt
    ProtectedKey      // scrambled = true, a key was set at T-12 of this attempt
};

const char* to_string(Protection protection);

class KeyManager {
public:
    static constexpr int kPesStartsToCheck = 3;   // REQ-51

    // Reported on the AL worker.
    struct Events {
        std::function<void(Protection)> on_video1;   // EV-VIDEO1
        std::function<void()> on_key_ok;             // EV-KEY-OK
        std::function<void()> on_key_bad;            // EV-KEY-BAD
        // Every packet of the four TS filters in the received order (S11, rule T3), decrypted when
        // a key is set; valid only during the call.
        std::function<void(const std::uint8_t* packet, bool scrambled)> on_packet;
    };

    KeyManager(pal::IDemux& demux, pal::ILogger& logger, TaskQueue& queue);
    ~KeyManager();

    KeyManager(const KeyManager&) = delete;
    KeyManager& operator=(const KeyManager&) = delete;

    // ---- key store (set_key of IF-INT-01, REQ-49, REQ-53, REQ-54)
    // Stores the key given as 32 hexadecimal characters; false (nothing stored) for another format.
    bool store(const std::string& service_id, const std::string& key_hex);
    bool has_key(const std::string& service_id) const { return keys_.contains(service_id); }
    std::optional<Key> lookup(const std::string& service_id) const { return keys_.lookup(service_id); }
    void discard(const std::string& service_id);   // REQ-53, called by AL-SES (T-17, T-06)
    void clear_all() { keys_.clear_all(); }        // orderly stop (Section 4.2.5)
    KeyStore& key_store() { return keys_; }         // test hook of TST-115

    // ---- data path of one stream attempt
    // S8 / T-12: IDemux::set_key for the video and audio PIDs if a key is stored for `service_id`,
    // then open and start the TS filters on PID 0, the PMT PID, the video PID and the audio PID if
    // present. A previous attempt is closed first. False if a filter cannot be opened or started
    // (logged; nothing is left open and the descrambler keys are cleared).
    bool start(std::uint64_t session, std::uint64_t generation, const std::string& service_id,
               const StreamDescription& stream, Events events);

    // RELEASE: closes the TS filters of the attempt and clears the descrambler keys; queued packets
    // of the attempt are discarded.
    void close();

    bool key_set() const { return key_set_; }
    int checked_pes_starts() const { return checked_; }

private:
    enum class KeyResult { Pending, Accepted, Rejected };

    void on_packet(std::uint64_t generation, const std::array<std::uint8_t, kTsPacketSize>& packet,
                   bool scrambled);
    void handle_video(std::uint64_t generation, const std::uint8_t* packet, bool scrambled);
    bool current(std::uint64_t generation) const { return active_ && generation == generation_; }
    void log(pal::LogLevel level, const std::string& message);

    pal::IDemux& demux_;
    pal::ILogger& logger_;
    TaskQueue& queue_;
    KeyStore keys_;

    bool active_ = false;
    std::uint64_t session_ = 0;
    std::uint64_t generation_ = 0;
    std::string service_id_;
    std::uint16_t video_pid_ = 0;
    Events events_;
    std::vector<pal::IDemux::FilterId> filters_;
    bool key_set_ = false;
    std::string key_mask_;   // mask of the key set at start (REQ-55)
    std::optional<Protection> protection_;
    KeyResult result_ = KeyResult::Pending;
    int checked_ = 0;
};

}  // namespace al
