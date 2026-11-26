#include "key_store.hpp"

#include <cstddef>

namespace al {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

constexpr const char* kEllipsis = "\xE2\x80\xA6";   // "…" in UTF-8

}  // namespace

std::optional<Key> parse_key_hex(const std::string& text) {
    if (text.size() != 32) return std::nullopt;
    Key key{};
    for (std::size_t i = 0; i < 16; ++i) {
        const int high = hex_value(text[2 * i]);
        const int low = hex_value(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            zero_key(key);
            return std::nullopt;
        }
        key[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return key;
}

std::string mask_key(const Key& key) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string text;
    for (std::size_t i = 0; i < 2; ++i) {
        text += kDigits[key[i] >> 4];
        text += kDigits[key[i] & 0x0F];
    }
    return text + kEllipsis;
}

void zero_key(Key& key) {
    volatile std::uint8_t* bytes = key.data();
    for (std::size_t i = 0; i < key.size(); ++i) bytes[i] = 0;
}

KeyStore::~KeyStore() { clear_all(); }

void KeyStore::store(const std::string& service_id, const Key& key) {
    auto it = keys_.find(service_id);
    if (it != keys_.end()) {
        zero_entry(it->second);
        it->second = key;
    } else {
        keys_.emplace(service_id, key);
    }
}

std::optional<Key> KeyStore::lookup(const std::string& service_id) const {
    auto it = keys_.find(service_id);
    if (it == keys_.end()) return std::nullopt;
    return it->second;
}

bool KeyStore::contains(const std::string& service_id) const { return keys_.count(service_id) != 0; }

void KeyStore::discard(const std::string& service_id) {
    auto it = keys_.find(service_id);
    if (it == keys_.end()) return;
    zero_entry(it->second);
    keys_.erase(it);
}

void KeyStore::clear_all() {
    for (auto& entry : keys_) zero_entry(entry.second);
    keys_.clear();
}

void KeyStore::zero_entry(Key& key) {
    zero_key(key);
    if (zeroed_hook_) zeroed_hook_(key);
}

}  // namespace al
