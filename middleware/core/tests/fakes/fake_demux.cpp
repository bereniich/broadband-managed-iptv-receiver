#include "fakes/fake_demux.hpp"

#include <gtest/gtest.h>

#include <fstream>

#include "content.hpp"
#include "fakes/reference_d04.hpp"

namespace fakes {

namespace {

constexpr std::size_t kPacketSize = pal::kTsPacketSize;
constexpr std::uint16_t kMaxPid = 0x1FFF;

std::uint16_t pid_of(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(((p[1] & 0x1F) << 8) | p[2]);
}

std::string type_name(pal::FilterType type) {
    return type == pal::FilterType::Section ? "section" : "ts";
}

}  // namespace

pal::IDemux::FilterId FakeDemux::open_filter(const pal::FilterConfig& config, DataCallback on_data) {
    std::string entry = "open_filter " + type_name(config.type) + " pid=" + std::to_string(config.pid);
    if (config.type == pal::FilterType::Section) entry += " table_id=" + std::to_string(config.table_id);
    if (config.pid > kMaxPid || !on_data) {
        record(log_, entry + " -> 0");
        return 0;
    }
    const FilterId id = next_id_++;
    Filter filter;
    filter.config = config;
    filter.on_data = std::move(on_data);
    filters_.emplace(id, std::move(filter));
    record(log_, entry + " -> " + std::to_string(id));
    return id;
}

bool FakeDemux::start_filter(FilterId id) {
    record(log_, "start_filter " + std::to_string(id));
    auto it = filters_.find(id);
    if (it == filters_.end()) return false;
    it->second.started = true;
    it->second.assembling = false;
    it->second.section.clear();
    return true;
}

void FakeDemux::stop_filter(FilterId id) {
    record(log_, "stop_filter " + std::to_string(id));
    auto it = filters_.find(id);
    if (it != filters_.end()) it->second.started = false;
}

void FakeDemux::close_filter(FilterId id) {
    record(log_, "close_filter " + std::to_string(id));
    filters_.erase(id);
}

void FakeDemux::set_key(const std::uint16_t* pids, std::size_t pid_count, const std::uint8_t key[16]) {
    std::string entry = "set_key";
    std::array<std::uint8_t, 16> k{};
    for (std::size_t i = 0; i < 16; ++i) k[i] = key[i];
    for (std::size_t i = 0; i < pid_count; ++i) {
        keys_[pids[i]] = k;
        entry += (i == 0 ? " " : ",") + std::to_string(pids[i]);
    }
    record(log_, entry);   // the key bytes are never recorded (REQ-55)
}

void FakeDemux::clear_keys() {
    record(log_, "clear_keys");
    for (auto& entry : keys_) entry.second.fill(0);
    keys_.clear();
}

void FakeDemux::feed_packet(const std::uint8_t* in) {
    ++stats_.ts_packets;
    if (in[0] != 0x47) {
        ++stats_.sync_errors;
        return;
    }
    std::array<std::uint8_t, kPacketSize> packet;
    for (std::size_t i = 0; i < kPacketSize; ++i) packet[i] = in[i];
    const std::uint16_t pid = pid_of(packet.data());

    const std::uint8_t afc = (packet[3] >> 4) & 0x3;
    if (afc == 0x1 || afc == 0x3) {   // the continuity counter counts packets with a payload
        const std::uint8_t cc = packet[3] & 0x0F;
        auto last = last_cc_.find(pid);
        if (last != last_cc_.end() && cc != ((last->second + 1) & 0x0F)) ++stats_.cc_errors;
        last_cc_[pid] = cc;
    }

    // Descrambler (REQ-50): '10' with a key -> decrypted, bits '00'; scrambled reports the received bits.
    const bool scrambled = ((packet[3] >> 6) & 0x3) != 0;
    auto key = keys_.find(pid);
    if (key != keys_.end()) d04_decrypt_packet(key->second, packet.data());

    // Snapshot of the ids, so that a callback may close filters.
    std::vector<FilterId> ids;
    for (const auto& entry : filters_) {
        if (entry.second.config.pid == pid && entry.second.started) ids.push_back(entry.first);
    }
    for (FilterId id : ids) {
        auto it = filters_.find(id);
        if (it == filters_.end() || !it->second.started) continue;
        if (it->second.config.type == pal::FilterType::Ts) {
            const std::array<std::uint8_t, kPacketSize> copy = packet;   // each filter its own copy
            it->second.on_data(FilterData{id, copy.data(), copy.size(), scrambled});
        } else {
            const std::size_t o = ts_payload_offset(packet.data());
            if (o != 0) deliver_sections(id, packet.data() + o, kPacketSize - o, (packet[1] & 0x40) != 0);
        }
    }
}

void FakeDemux::feed(const std::vector<std::uint8_t>& ts) {
    for (std::size_t offset = 0; offset + kPacketSize <= ts.size(); offset += kPacketSize) {
        feed_packet(ts.data() + offset);
    }
}

std::size_t FakeDemux::feed_file(const std::string& path, std::size_t first, std::size_t count) {
    if (!testutil::content_exists(path)) return 0;
    std::ifstream file(path, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(first * kPacketSize));
    std::array<std::uint8_t, kPacketSize> packet;
    std::size_t fed = 0;
    while (fed < count && file.read(reinterpret_cast<char*>(packet.data()), kPacketSize)) {
        feed_packet(packet.data());
        ++fed;
    }
    return fed;
}

std::vector<FakeDemux::OpenFilter> FakeDemux::open_filters() const {
    std::vector<OpenFilter> result;
    for (const auto& entry : filters_) {
        result.push_back(OpenFilter{entry.first, entry.second.config, entry.second.started});
    }
    return result;
}

// Section assembly (ISO/IEC 13818-1, 2.4.4.2): in a packet with payload_unit_start_indicator = 1 the
// first payload byte is pointer_field, the number of bytes that still belong to the previous section.
void FakeDemux::deliver_sections(FilterId id, const std::uint8_t* payload, std::size_t len, bool pusi) {
    Filter& f = filters_.at(id);
    if (pusi) {
        const std::size_t pointer = payload[0];
        if (1 + pointer > len) {   // invalid pointer_field: drop everything of this packet
            f.assembling = false;
            f.section.clear();
            return;
        }
        if (f.assembling) {
            f.section.insert(f.section.end(), payload + 1, payload + 1 + pointer);
            extract_sections(id);
            if (filters_.count(id) == 0) return;
        }
        Filter& g = filters_.at(id);
        g.section.assign(payload + 1 + pointer, payload + len);
        g.assembling = true;
        extract_sections(id);
    } else if (f.assembling) {
        f.section.insert(f.section.end(), payload, payload + len);
        extract_sections(id);
    }
}

void FakeDemux::extract_sections(FilterId id) {
    for (;;) {
        auto it = filters_.find(id);
        if (it == filters_.end()) return;
        Filter& f = it->second;
        if (!f.assembling) return;
        if (f.section.empty() || f.section[0] == 0xFF) {   // end of the data or stuffing
            f.assembling = false;
            f.section.clear();
            return;
        }
        if (f.section.size() < 3) return;
        const std::size_t total = 3 + (((f.section[1] & 0x0F) << 8) | f.section[2]);
        if (total > kMaxSectionSize) {   // B3: discarded, assembly restarts at the next pusi
            f.assembling = false;
            f.section.clear();
            return;
        }
        if (f.section.size() < total) return;
        const std::vector<std::uint8_t> complete(f.section.begin(),
                                                 f.section.begin() + static_cast<std::ptrdiff_t>(total));
        f.section.erase(f.section.begin(), f.section.begin() + static_cast<std::ptrdiff_t>(total));
        if (f.config.table_id < 0 || complete[0] == f.config.table_id) {
            const DataCallback callback = f.on_data;   // the callback may close the filter
            callback(FilterData{id, complete.data(), complete.size(), false});
        }
    }
}

}  // namespace fakes
