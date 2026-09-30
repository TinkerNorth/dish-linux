// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A satellite session's own account of itself, as the diagnostics page reads it:
// the heartbeat ack's epoch and controller bitmap against what this client last
// applied, whether the host reports one controller streaming, and which
// capabilities a controller's descriptor advertised. Every answer the ack gives
// has an Unknown, because a session is live before its first enriched ack.

#pragma once

#include "core/model/Protocol.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dish::reducer {

// SatelliteClient's ack fields read -1 until the first enriched heartbeat ack.
inline bool ackArrived(int ackField) { return ackField >= 0; }

// Whether the host agrees with this client about one thing: Unknown until the
// first enriched ack says.
enum class Agreement : std::uint8_t { Unknown, InStep, Diverged };

inline Agreement epochAgreement(int hostEpoch, int appliedEpoch) {
    if (!ackArrived(hostEpoch)) { return Agreement::Unknown; }
    const bool same = hostEpoch == appliedEpoch;
    return same ? Agreement::InStep : Agreement::Diverged;
}

// The controllers the host reports active against the ones it confirmed at
// the last apply.
inline Agreement bitmapAgreement(int hostBitmap, std::uint16_t confirmedBitmap) {
    if (!ackArrived(hostBitmap)) { return Agreement::Unknown; }
    const bool same = static_cast<std::uint16_t>(hostBitmap) == confirmedBitmap;
    return same ? Agreement::InStep : Agreement::Diverged;
}

inline constexpr int kControllerBitmapWidth = 16;

inline bool bitmapHasIndex(std::uint16_t bitmap, int index) {
    const bool inRange = index >= 0 && index < kControllerBitmapWidth;
    if (!inRange) { return false; }
    const auto bit = static_cast<std::uint16_t>(1U << static_cast<unsigned>(index));
    return (bitmap & bit) != 0;
}

// The controller indices a bitmap names, lowest first.
inline std::vector<int> bitmapIndices(std::uint16_t bitmap) {
    std::vector<int> indices;
    for (int index = 0; index < kControllerBitmapWidth; ++index) {
        if (bitmapHasIndex(bitmap, index)) { indices.push_back(index); }
    }
    return indices;
}

// The controllers the host reports active: none before the first enriched ack,
// when the agreement reads Unknown.
inline std::vector<int> hostControllerIndices(int hostBitmap) {
    if (!ackArrived(hostBitmap)) { return {}; }
    return bitmapIndices(static_cast<std::uint16_t>(hostBitmap));
}

enum class Streaming : std::uint8_t { Unknown, Yes, No };

// Whether the host reports this controller index active.
inline Streaming streamingOf(int hostBitmap, int controllerIndex) {
    if (!ackArrived(hostBitmap)) { return Streaming::Unknown; }
    const bool active = bitmapHasIndex(static_cast<std::uint16_t>(hostBitmap), controllerIndex);
    return active ? Streaming::Yes : Streaming::No;
}

// The ack's backend byte: whether the host can create virtual controllers.
enum class HostBackend : std::uint8_t { Unknown, Available, Unavailable };

inline HostBackend hostBackendOf(int backendAvailable) {
    if (!ackArrived(backendAvailable)) { return HostBackend::Unknown; }
    return backendAvailable != 0 ? HostBackend::Available : HostBackend::Unavailable;
}

// A capability a descriptor's caps word can advertise, in wire-bit order.
enum class WireCap : std::uint8_t {
    AnalogTriggers,
    Rumble,
    Motion,
    Lightbar,
    TriggerEffects,
    PlayerLeds,
    Mic,
    Speaker,
    HapticAudio,
};

struct WireCapBit {
    std::uint16_t bit = 0;
    WireCap cap = WireCap::AnalogTriggers;
};

inline constexpr std::array<WireCapBit, 9> kWireCapBits{{
    {proto::kCapAnalogTriggers, WireCap::AnalogTriggers},
    {proto::kCapRumble, WireCap::Rumble},
    {proto::kCapMotion, WireCap::Motion},
    {proto::kCapLightbar, WireCap::Lightbar},
    {proto::kCapTriggerEffects, WireCap::TriggerEffects},
    {proto::kCapPlayerLeds, WireCap::PlayerLeds},
    {proto::kCapMic, WireCap::Mic},
    {proto::kCapSpeaker, WireCap::Speaker},
    {proto::kCapHapticAudio, WireCap::HapticAudio},
}};

inline std::vector<WireCap> advertisedCaps(std::uint16_t caps) {
    std::vector<WireCap> advertised;
    for (const auto& entry : kWireCapBits) {
        if ((caps & entry.bit) != 0) { advertised.push_back(entry.cap); }
    }
    return advertised;
}

} // namespace dish::reducer
