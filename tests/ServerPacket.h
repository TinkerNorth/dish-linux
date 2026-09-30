// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A datagram as the satellite seals it for this client: token, big-endian
// counter, then the AEAD over type, length and body. It is what lets a test
// drive the receive dispatch through SatelliteClientTestAccess::processIncoming
// without a socket.

#pragma once

#include "Util/Endian.h"
#include "core/wire/SessionCrypto.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dish::test {

inline constexpr std::size_t kServerPacketHeaderBytes = 8;
inline constexpr std::size_t kServerPacketInnerHeaderBytes = 4;
inline constexpr std::size_t kServerPacketTagBytes = 16;

// Empty when the AEAD refused, which the caller REQUIREs against.
inline std::vector<std::uint8_t> sealServerPacket(const std::array<std::uint8_t, 4>& token,
                                                  const std::array<std::uint8_t, 32>& key,
                                                  std::uint16_t msgType,
                                                  const std::vector<std::uint8_t>& body,
                                                  std::uint32_t counter) {
    std::vector<std::uint8_t> inner(kServerPacketInnerHeaderBytes + body.size());
    util::putU16Be(inner.data(), msgType);
    util::putU16Be(inner.data() + 2, static_cast<std::uint16_t>(body.size()));
    if (!body.empty()) {
        std::memcpy(inner.data() + kServerPacketInnerHeaderBytes, body.data(), body.size());
    }

    std::vector<std::uint8_t> pkt(kServerPacketHeaderBytes + inner.size() + kServerPacketTagBytes);
    std::memcpy(pkt.data(), token.data(), token.size());
    util::putU32Be(pkt.data() + token.size(), counter);
    const std::uint32_t tokenBe = util::readU32Be(token.data());
    unsigned long long sealedLen = 0;
    const bool sealed =
        wire::encryptPacket(key.data(), wire::kDirServerToClient, counter, tokenBe, inner.data(),
                            inner.size(), pkt.data() + kServerPacketHeaderBytes, &sealedLen);
    if (!sealed) { return {}; }
    pkt.resize(kServerPacketHeaderBytes + static_cast<std::size_t>(sealedLen));
    return pkt;
}

} // namespace dish::test
