// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The declarative session's self-heal loop. Every enriched heartbeat ack carries
// (epoch, activeBitmap); when either drifts from what was last applied, GET
// /api/connections/{id}. If the applied view still matches desired the drift was
// benign and only the epoch is adopted, otherwise the full desired state is
// re-PUT. See satellite/docs/contract.md, Session and Enriched heartbeat ack.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace dish::reducer {

// touchpadMode is part of the comparison key because a server-side mode reset
// would otherwise be invisible to the loop and touchpad packets would die
// silently.
struct DesiredSlot {
    std::uint8_t ctrlIdx = 0;
    std::uint8_t type = 0;
    std::uint8_t touchpadMode = 2; // proto::kTouchpadModeOff
};

// A nullopt touchpadMode means the server predates reporting it, so the
// comparison skips the mode arm rather than forcing an endless re-PUT.
struct AppliedSlot {
    std::uint8_t ctrlIdx = 0;
    std::uint8_t appliedType = 0;
    bool active = true;
    std::optional<std::uint8_t> touchpadMode;
};

// Bit ctrlIdx set per registered slot, to compare against the ack's activeBitmap.
inline std::uint16_t expectedBitmap(const std::vector<DesiredSlot>& registered) {
    std::uint16_t bitmap = 0;
    for (const auto& s : registered) {
        if (s.ctrlIdx <= 15) { bitmap = static_cast<std::uint16_t>(bitmap | (1u << s.ctrlIdx)); }
    }
    return bitmap;
}

// A negative `serverEpoch` means no enriched ack has been seen yet; a negative
// `serverBitmap` means unknown, so that arm is skipped.
inline bool reconcileNeeded(int serverEpoch, int serverBitmap, int lastAppliedEpoch,
                            std::uint16_t expected) {
    if (serverEpoch < 0) { return false; }
    if (serverEpoch != lastAppliedEpoch) { return true; }
    if (serverBitmap >= 0 && static_cast<std::uint16_t>(serverBitmap) != expected) { return true; }
    return false;
}

// Only `active` controllers count on the applied side; an inactive slot is
// unplugged server-side. `mouseWantsVsGrantedMatch` folds in the host-feature
// grant: the grant is only computed at session PUT, so a slot toggled to mouse
// mid-session stays mismatched until a re-PUT and must force the converge.
inline bool appliedMatchesDesired(const std::vector<DesiredSlot>& desired,
                                  const std::vector<AppliedSlot>& applied,
                                  bool mouseWantsVsGrantedMatch = true) {
    if (!mouseWantsVsGrantedMatch) { return false; }
    std::map<std::uint8_t, DesiredSlot> want;
    for (const auto& d : desired) { want[d.ctrlIdx] = d; }
    std::size_t activeCount = 0;
    for (const auto& a : applied) {
        if (!a.active) { continue; }
        ++activeCount;
        const auto it = want.find(a.ctrlIdx);
        if (it == want.end()) { return false; }
        if (it->second.type != a.appliedType) { return false; }
        if (a.touchpadMode.has_value() && *a.touchpadMode != it->second.touchpadMode) {
            return false;
        }
    }
    return activeCount == want.size();
}

// ── Late-slot converge ──────────────────────────────────────────────────────
// Keyed on (ctrlIdx, type) only. LateSlotConverge.h holds the stricter
// whole-descriptor variant.
struct LateConverge {
    std::vector<std::uint8_t> resyncs; // ctrlIdx to PUT /controllers/{idx}
    std::vector<std::uint8_t> removes; // ctrlIdx to DELETE /controllers/{idx}
};

inline LateConverge lateSlotConverge(const std::vector<DesiredSlot>& sent,
                                     const std::vector<DesiredSlot>& desired) {
    LateConverge out;
    std::map<std::uint8_t, std::uint8_t> sentByIdx;
    for (const auto& s : sent) { sentByIdx[s.ctrlIdx] = s.type; }
    std::map<std::uint8_t, std::uint8_t> desiredByIdx;
    for (const auto& d : desired) { desiredByIdx[d.ctrlIdx] = d.type; }

    for (const auto& d : desired) {
        const auto it = sentByIdx.find(d.ctrlIdx);
        if (it == sentByIdx.end() || it->second != d.type) { out.resyncs.push_back(d.ctrlIdx); }
    }
    for (const auto& s : sent) {
        if (desiredByIdx.find(s.ctrlIdx) == desiredByIdx.end()) {
            out.removes.push_back(s.ctrlIdx);
        }
    }
    std::sort(out.resyncs.begin(), out.resyncs.end());
    std::sort(out.removes.begin(), out.removes.end());
    return out;
}

// ── Send-counter exhaustion guard ───────────────────────────────────────────
// The UDP send counter can never wrap. A session approaching 2^32 self-heals by
// re-PUTting for a fresh token/salt/key with the counter back at 1; the contract
// sets the trigger at 0xF0000000.
inline constexpr std::uint32_t kCounterRepushThreshold = 0xF0000000u;

inline bool counterNeedsRepush(std::uint32_t sendCounter) {
    return sendCounter >= kCounterRepushThreshold;
}

// Largest counter the 4-byte wire field can carry.
inline constexpr std::uint64_t kCounterWireMax = 0xFFFFFFFFull;

// nullopt once the space is exhausted, at which point the sender goes silent:
// sealing a second plaintext under one (key, nonce) leaks keystream.
inline std::optional<std::uint32_t> wireSendCounter(std::uint64_t sequence) {
    if (sequence > kCounterWireMax) { return std::nullopt; }
    return static_cast<std::uint32_t>(sequence);
}

// Clamped, not truncated: past exhaustion the repush poll must keep reading
// "re-PUT needed" rather than wrapping back under the threshold.
inline std::uint32_t clampedSendCounter(std::uint64_t sequence) {
    return static_cast<std::uint32_t>(std::min(sequence, kCounterWireMax));
}

} // namespace dish::reducer
