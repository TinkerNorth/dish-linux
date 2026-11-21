// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The session's own count of what it put on the wire and what came back, per
// message kind (docs/diagnostics-bundle.md, "The hosts, from both ends":
// `session.sent` and `session.received`). Counted where the datagram leaves
// and where it is dispatched, which is the input, USB, mic, heartbeat and
// receive threads, so a count is a relaxed atomic add and nothing else: those
// threads may not take a lock (the measurements workflow fails if they do).

#pragma once

#include "core/model/Protocol.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dish::diagnostics {

enum class SentKind : std::uint8_t { Input, Motion, Battery, Pointer, Mic };
enum class ReceivedKind : std::uint8_t {
    Rumble,
    Lightbar,
    TriggerEffects,
    PlayerLeds,
    MicLed,
    Speaker,
    Haptic,
    SessionClose,
};

template <typename Kind> struct WireKindRow {
    std::uint16_t msgType = 0;
    Kind kind{};
    std::string_view token;
};

// The bundle's names, in its order. The heartbeat and its ack are not here: the ack's facts are
// `session.ack`, and a ping every two seconds says nothing a count would add.
inline constexpr std::array<WireKindRow<SentKind>, 5> kSentKinds{{
    {proto::kMsgInput, SentKind::Input, "input"},
    {proto::kMsgMotion, SentKind::Motion, "motion"},
    {proto::kMsgBattery, SentKind::Battery, "battery"},
    {proto::kMsgTouchpad, SentKind::Pointer, "pointer"},
    {proto::kMsgMicAudio, SentKind::Mic, "mic"},
}};

inline constexpr std::array<WireKindRow<ReceivedKind>, 8> kReceivedKinds{{
    {proto::kMsgRumble, ReceivedKind::Rumble, "rumble"},
    {proto::kMsgLightbar, ReceivedKind::Lightbar, "lightbar"},
    {proto::kMsgTriggerEffects, ReceivedKind::TriggerEffects, "triggerEffects"},
    {proto::kMsgPlayerLeds, ReceivedKind::PlayerLeds, "playerLeds"},
    {proto::kMsgMicLed, ReceivedKind::MicLed, "micLed"},
    {proto::kMsgSpeakerAudio, ReceivedKind::Speaker, "speaker"},
    {proto::kMsgHapticAudio, ReceivedKind::Haptic, "haptic"},
    {proto::kMsgSessionClose, ReceivedKind::SessionClose, "sessionClose"},
}};

// The row a message type counts under; empty for one the table does not name.
template <typename Kind, std::size_t N>
constexpr std::optional<std::size_t> wireKindIndex(const std::array<WireKindRow<Kind>, N>& rows,
                                                   std::uint16_t msgType) {
    for (std::size_t i = 0; i < N; ++i) {
        if (rows[i].msgType == msgType) { return i; }
    }
    return std::nullopt;
}

struct WireCounts {
    std::array<std::uint64_t, kSentKinds.size()> sent{};
    std::array<std::uint64_t, kReceivedKinds.size()> received{};

    bool operator==(const WireCounts&) const = default;
};

class WireCounters {
  public:
    void countSent(std::uint16_t msgType) noexcept {
        bump(sent_, wireKindIndex(kSentKinds, msgType));
    }
    void countReceived(std::uint16_t msgType) noexcept {
        bump(received_, wireKindIndex(kReceivedKinds, msgType));
    }

    // Each count read on its own: a snapshot taken while frames flow is a moment, not a ledger.
    WireCounts counts() const noexcept {
        WireCounts out;
        for (std::size_t i = 0; i < out.sent.size(); ++i) {
            out.sent[i] = sent_[i].load(std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < out.received.size(); ++i) {
            out.received[i] = received_[i].load(std::memory_order_relaxed);
        }
        return out;
    }

  private:
    template <std::size_t N>
    static void bump(std::array<std::atomic<std::uint64_t>, N>& cells,
                     std::optional<std::size_t> index) noexcept {
        if (!index.has_value()) { return; }
        cells[*index].fetch_add(1, std::memory_order_relaxed);
    }

    std::array<std::atomic<std::uint64_t>, kSentKinds.size()> sent_{};
    std::array<std::atomic<std::uint64_t>, kReceivedKinds.size()> received_{};
};

} // namespace dish::diagnostics
