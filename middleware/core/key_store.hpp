// Key store of AL-DSC (Section 4.2.3): service_id -> 16-byte AES-128 key, in memory only (D-03).
// Key bytes are overwritten with zeros when an entry is removed or replaced and when the store is
// cleared or destroyed. Keys are never logged in full (REQ-55): mask_key() gives at most 4 characters.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace al {

using Key = std::array<std::uint8_t, 16>;

// 32 hexadecimal characters, case-insensitive ([0-9A-Fa-f]{32}, Section 3.1) -> 16 bytes;
// nullopt for any other text (INVALID_KEY_FORMAT).
std::optional<Key> parse_key_hex(const std::string& text);

// First 4 hexadecimal characters of the key in upper case followed by "…", e.g. "3A7F…".
std::string mask_key(const Key& key);

// Overwrites the key with zeros through a volatile pointer, so that the compiler cannot remove the
// stores although the memory is released afterwards.
void zero_key(Key& key);

class KeyStore {
public:
    KeyStore() = default;
    KeyStore(const KeyStore&) = delete;
    KeyStore& operator=(const KeyStore&) = delete;
    ~KeyStore();

    void store(const std::string& service_id, const Key& key);   // replaces an existing key
    std::optional<Key> lookup(const std::string& service_id) const;
    bool contains(const std::string& service_id) const;
    void discard(const std::string& service_id);   // REQ-53
    void clear_all();                              // orderly stop (Section 4.2.5)
    std::size_t size() const { return keys_.size(); }

    // Test hook (TST-115): called with the stored bytes right after they were overwritten with zeros.
    void set_zeroed_hook(std::function<void(const Key&)> hook) { zeroed_hook_ = std::move(hook); }

private:
    void zero_entry(Key& key);

    std::map<std::string, Key> keys_;
    std::function<void(const Key&)> zeroed_hook_;
};

}  // namespace al
