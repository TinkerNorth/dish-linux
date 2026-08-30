// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// AES-128-GCM sealing for the Moonlight control stream. Every control message
// travels as an ENCRYPTED packet:
//
//   [type u16 LE = 0x0001][len u16 LE][seq u32 LE][GCM tag 16B][ciphertext]
//
// where len covers seq + tag + ciphertext. The key is the launch request's
// rikey; the IV is 16 bytes of zero with iv[0] = seq & 0xFF — exactly the
// construction Wolf's control.hpp uses against real clients (the seq is
// truncated to one byte on both ends, so the construction only matters that it
// MATCHES; see decrypt_packet there). The unit tests pin seal() against
// packets captured from a real session.
//
// Both EVP contexts are allocated once and reused per packet, so the hot path
// performs no per-packet heap allocation inside this class.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// OpenSSL's EVP_CIPHER_CTX without pulling evp.h into every includer.
struct evp_cipher_ctx_st;

namespace dish::mooncrypto {

class ControlCipher {
  public:
    // Packet framing constants.
    static constexpr std::size_t kHeaderSize = 4; // type + len
    static constexpr std::size_t kSeqSize = 4;
    static constexpr std::size_t kTagSize = 16;
    static constexpr std::size_t kOverhead = kHeaderSize + kSeqSize + kTagSize;

    ControlCipher();
    ~ControlCipher();

    ControlCipher(const ControlCipher&) = delete;
    ControlCipher& operator=(const ControlCipher&) = delete;
    ControlCipher(ControlCipher&&) = delete;
    ControlCipher& operator=(ControlCipher&&) = delete;

    // Installs the 16-byte rikey and prepares both directions. Must succeed
    // before seal/open; returns false on an OpenSSL failure.
    bool setKey(const std::array<std::uint8_t, 16>& key);
    bool hasKey() const { return keySet_; }

    // Seals `plaintext` into a full ENCRYPTED packet at `out`, which must have
    // room for ptLen + kOverhead bytes. Returns the total packet length, or 0
    // on failure (no key, oversized, or OpenSSL error).
    std::size_t seal(std::uint32_t seq, const std::uint8_t* plaintext, std::size_t ptLen,
                     std::uint8_t* out);

    // Opens a full ENCRYPTED packet: parses the framing, verifies the GCM tag
    // and writes the plaintext into `out` (capacity `outCap`). nullopt on a
    // short/misframed packet, a failed authentication, or a too-small buffer.
    std::optional<std::size_t> open(const std::uint8_t* packet, std::size_t len, std::uint8_t* out,
                                    std::size_t outCap);

  private:
    evp_cipher_ctx_st* encCtx_ = nullptr;
    evp_cipher_ctx_st* decCtx_ = nullptr;
    bool keySet_ = false;
};

} // namespace dish::mooncrypto
