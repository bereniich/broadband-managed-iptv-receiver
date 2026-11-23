// Reference implementation of the protection scheme D-04 for the fake demux and the AL unit tests,
// written in the test code from the text of D-04 and Section 4.7.3 (Section 2.2.7; not taken from
// HAL-DSC or the packaging tool). AES-128 follows FIPS-197; CBC mode follows NIST SP 800-38A.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fakes {

using AesBlock = std::array<std::uint8_t, 16>;
using AesKey = std::array<std::uint8_t, 16>;

AesBlock aes128_encrypt_block(const AesKey& key, const AesBlock& plaintext);
AesBlock aes128_decrypt_block(const AesKey& key, const AesBlock& ciphertext);

// CBC over `len` bytes in place; `len` must be a multiple of 16.
void cbc_encrypt(const AesKey& key, const AesBlock& iv, std::uint8_t* data, std::size_t len);
void cbc_decrypt(const AesKey& key, const AesBlock& iv, std::uint8_t* data, std::size_t len);

// Offset of the payload of a 188-byte TS packet ('01': 4, '11': 5 + adaptation_field_length),
// or 0 when the packet has no payload ('00', '10', or an adaptation field that fills the packet).
std::size_t ts_payload_offset(const std::uint8_t* packet);

// D-04 on one 188-byte packet in place. Encryption: only a packet with at least 16 payload bytes is
// changed — the largest multiple of 16 bytes from the payload start is encrypted with AES-128-CBC,
// IV = 16 zero bytes, the remaining bytes stay clear, transport_scrambling_control becomes '10'.
// Decryption: only a packet with '10' is changed — the same bytes are decrypted, the bits become '00'.
// Both return true if the packet was changed.
bool d04_encrypt_packet(const AesKey& key, std::uint8_t* packet);
bool d04_decrypt_packet(const AesKey& key, std::uint8_t* packet);

}  // namespace fakes
