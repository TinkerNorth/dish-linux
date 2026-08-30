// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/moonlight/MoonlightControlCipher.h"

#include "core/moonlight/MoonlightProtocol.h"

#include <openssl/evp.h>

#include <cstring>

namespace dish::mooncrypto {
namespace {

// The wire caps len at u16; anything this client sends is far below it.
constexpr std::size_t kMaxPlaintext = 1024;
constexpr std::size_t kIvSize = 16;

void putU16Le(std::uint8_t* dst, std::uint16_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
}

void putU32Le(std::uint8_t* dst, std::uint32_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    dst[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
}

std::uint16_t readU16Le(const std::uint8_t* src) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(src[0]) |
                                      (static_cast<std::uint16_t>(src[1]) << 8));
}

std::uint32_t readU32Le(const std::uint8_t* src) noexcept {
    return static_cast<std::uint32_t>(src[0]) | (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) | (static_cast<std::uint32_t>(src[3]) << 24);
}

// 16 zero bytes with only the low byte of seq in iv[0]. Wolf assigns the u32
// seq to a uint8_t slot, and interop with real clients proves both ends do the
// same, so this deliberately truncates rather than spreading seq over 4 bytes.
void buildIv(std::uint32_t seq, std::uint8_t iv[kIvSize]) noexcept {
    std::memset(iv, 0, kIvSize);
    iv[0] = static_cast<std::uint8_t>(seq & 0xFFU);
}

} // namespace

ControlCipher::ControlCipher() = default;

ControlCipher::~ControlCipher() {
    if (encCtx_ != nullptr) { EVP_CIPHER_CTX_free(encCtx_); }
    if (decCtx_ != nullptr) { EVP_CIPHER_CTX_free(decCtx_); }
}

bool ControlCipher::setKey(const std::array<std::uint8_t, 16>& key) {
    keySet_ = false;
    if (encCtx_ == nullptr) { encCtx_ = EVP_CIPHER_CTX_new(); }
    if (decCtx_ == nullptr) { decCtx_ = EVP_CIPHER_CTX_new(); }
    if (encCtx_ == nullptr || decCtx_ == nullptr) { return false; }

    // Bind cipher + key once; per-packet calls below re-init with only the IV.
    if (EVP_EncryptInit_ex(encCtx_, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(encCtx_, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvSize), nullptr) !=
            1 ||
        EVP_EncryptInit_ex(encCtx_, nullptr, nullptr, key.data(), nullptr) != 1 ||
        EVP_CIPHER_CTX_set_padding(encCtx_, 0) != 1) {
        return false;
    }
    if (EVP_DecryptInit_ex(decCtx_, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(decCtx_, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvSize), nullptr) !=
            1 ||
        EVP_DecryptInit_ex(decCtx_, nullptr, nullptr, key.data(), nullptr) != 1 ||
        EVP_CIPHER_CTX_set_padding(decCtx_, 0) != 1) {
        return false;
    }
    keySet_ = true;
    return true;
}

std::size_t ControlCipher::seal(std::uint32_t seq, const std::uint8_t* plaintext, std::size_t ptLen,
                                std::uint8_t* out) {
    if (!keySet_ || plaintext == nullptr || out == nullptr || ptLen == 0 || ptLen > kMaxPlaintext) {
        return 0;
    }

    std::uint8_t iv[kIvSize];
    buildIv(seq, iv);
    if (EVP_EncryptInit_ex(encCtx_, nullptr, nullptr, nullptr, iv) != 1) { return 0; }

    std::uint8_t* ct = out + kHeaderSize + kSeqSize + kTagSize;
    int ctLen = 0;
    if (EVP_EncryptUpdate(encCtx_, ct, &ctLen, plaintext, static_cast<int>(ptLen)) != 1) {
        return 0;
    }
    int finalLen = 0;
    if (EVP_EncryptFinal_ex(encCtx_, ct + ctLen, &finalLen) != 1) { return 0; }
    const std::size_t cipherLen =
        static_cast<std::size_t>(ctLen) + static_cast<std::size_t>(finalLen);
    if (EVP_CIPHER_CTX_ctrl(encCtx_, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagSize),
                            out + kHeaderSize + kSeqSize) != 1) {
        return 0;
    }

    putU16Le(out, moonproto::kPktEncrypted);
    putU16Le(out + 2, static_cast<std::uint16_t>(kSeqSize + kTagSize + cipherLen));
    putU32Le(out + kHeaderSize, seq);
    return kHeaderSize + kSeqSize + kTagSize + cipherLen;
}

std::optional<std::size_t> ControlCipher::open(const std::uint8_t* packet, std::size_t len,
                                               std::uint8_t* out, std::size_t outCap) {
    if (!keySet_ || packet == nullptr || out == nullptr || len < kOverhead) { return std::nullopt; }
    if (readU16Le(packet) != moonproto::kPktEncrypted) { return std::nullopt; }
    const std::size_t declared = readU16Le(packet + 2);
    if (declared < kSeqSize + kTagSize || declared + kHeaderSize > len) { return std::nullopt; }
    const std::uint32_t seq = readU32Le(packet + kHeaderSize);
    const std::uint8_t* tag = packet + kHeaderSize + kSeqSize;
    const std::uint8_t* ct = tag + kTagSize;
    const std::size_t ctLen = declared - kSeqSize - kTagSize;
    if (ctLen > outCap) { return std::nullopt; }

    std::uint8_t iv[kIvSize];
    buildIv(seq, iv);
    if (EVP_DecryptInit_ex(decCtx_, nullptr, nullptr, nullptr, iv) != 1) { return std::nullopt; }
    int ptLen = 0;
    if (ctLen > 0 && EVP_DecryptUpdate(decCtx_, out, &ptLen, ct, static_cast<int>(ctLen)) != 1) {
        return std::nullopt;
    }
    // SET_TAG's argument is const-correct only from OpenSSL 3; the copy keeps
    // the API const on our side.
    std::uint8_t tagCopy[kTagSize];
    std::memcpy(tagCopy, tag, kTagSize);
    if (EVP_CIPHER_CTX_ctrl(decCtx_, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagSize), tagCopy) !=
        1) {
        return std::nullopt;
    }
    int finalLen = 0;
    if (EVP_DecryptFinal_ex(decCtx_, out + ptLen, &finalLen) != 1) { return std::nullopt; }
    return static_cast<std::size_t>(ptLen) + static_cast<std::size_t>(finalLen);
}

} // namespace dish::mooncrypto
