// HAL-DSC: software descrambler of the protection scheme D-04 (REQ-50), over OpenSSL libcrypto.
//
// A packet of a PID with a key and transport_scrambling_control = '10' is decrypted in place: the
// payload after the TS header and the adaptation field, the largest multiple of 16 bytes, AES-128-CBC
// with an IV of 16 zero bytes restarted for every packet; the remaining bytes stay unchanged; the
// scrambling bits become '00'. Other packets are not touched. The descrambler is called by hal::Demux
// on the tuner receive thread, under the demux mutex (Section 4.5.2), and shares no code with the
// packaging tool SRV-PKG (rule R5).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

namespace hal {

class Descrambler {
public:
    Descrambler() = default;
    ~Descrambler();
    Descrambler(const Descrambler&) = delete;
    Descrambler& operator=(const Descrambler&) = delete;

    // Sets (or replaces) the key of each PID; PIDs above 0x1FFF are ignored. Returns false if the
    // cipher cannot be prepared.
    bool set_key(const std::uint16_t* pids, std::size_t pid_count, const std::uint8_t key[16]);
    // Removes all keys; their bytes are overwritten.
    void clear_keys();
    bool has_key(std::uint16_t pid) const { return keys_.count(pid) != 0; }

    // D-04 on one 188-byte TS packet. Returns true if the packet was decrypted (and its bits set to '00').
    bool descramble(std::uint8_t* packet);

    // The internal AES-128-CBC decryption with the IV as a parameter, no padding; `len` is a multiple of
    // 16 (Section 4.1.4; checked with the vectors of FIPS-197 and NIST SP 800-38A in TST-215).
    // `in` and `out` may be the same buffer. Returns false for invalid arguments or a cipher error.
    static bool aes128_cbc_decrypt(const std::uint8_t key[16], const std::uint8_t iv[16], const std::uint8_t* in,
                                   std::uint8_t* out, std::size_t len);

private:
    struct Key;
    std::map<std::uint16_t, std::shared_ptr<Key>> keys_;  // PIDs of one set_key share one key
};

}  // namespace hal
