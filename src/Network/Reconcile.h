// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pure reconcile decision logic for the declarative session (contract §Session
// / §Enriched heartbeat ack). Free functions, Qt-free, socket-free — the
// protocol's self-heal rules as documentation. Ported from dish-windows
// core/reducer/Reconcile.h.
//
// The loop: every enriched ack carries (epoch, activeBitmap). When either
// drifts from what we last applied, GET /api/connections/{id}; if the applied
// view still matches desired, just adopt the new epoch (benign drift); else
// re-PUT the full desired state (self-heal ≤2s).

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace dish::reducer {

// One desired controller slot, reduced to what reconcile compares on: index +
// emulation type. (caps/touchpadMode also converge via re-PUT, but the
// server's applied view reports type, so type is the comparison key.)
struct DesiredSlot {
    std::uint8_t ctrlIdx = 0;
    std::uint8_t type = 0;
};

// One applied controller from a GET /api/connections/{id} response.
struct AppliedSlot {
    std::uint8_t ctrlIdx = 0;
    std::uint8_t appliedType = 0;
    bool active = true;
};

// The 16-bit active-controller bitmap the client expects, derived from the
// slots it believes are registered/live (bit ctrlIdx set). Compared against the
// enriched-ack activeBitmap.
inline std::uint16_t expectedBitmap(const std::vector<DesiredSlot>& registered) {
    std::uint16_t bitmap = 0;
    for (const auto& s : registered) {
        if (s.ctrlIdx <= 15) { bitmap = static_cast<std::uint16_t>(bitmap | (1u << s.ctrlIdx)); }
    }
    return bitmap;
}

// Does the enriched ack (serverEpoch, serverBitmap) indicate the server's
// applied topology drifted from ours? `serverEpoch < 0` means no enriched ack
// has been seen yet (don't reconcile). A `serverBitmap < 0` means unknown
// (skip the bitmap arm). Returns true when a GET-then-maybe-rePUT is warranted.
inline bool reconcileNeeded(int serverEpoch, int serverBitmap, int lastAppliedEpoch,
                            std::uint16_t expected) {
    if (serverEpoch < 0) { return false; }
    if (serverEpoch != lastAppliedEpoch) { return true; }
    if (serverBitmap >= 0 && static_cast<std::uint16_t>(serverBitmap) != expected) { return true; }
    return false;
}

// After GET: does the server's applied view match our desired set? When it
// does, the drift was benign (e.g. our own standalone PUT raced an ack) and the
// caller just adopts the new epoch; when it doesn't, the caller re-PUTs. Only
// `active` controllers count on the applied side (an inactive slot is unplugged
// server-side). `mouseWantsVsGrantedMatch` folds the host-feature grant in: a
// slot toggled to mouse mid-session leaves wants≠granted until a re-PUT (the
// grant is only computed at session PUT — contract §hostFeatures), so a
// mismatch here also forces the converge.
inline bool appliedMatchesDesired(const std::vector<DesiredSlot>& desired,
                                  const std::vector<AppliedSlot>& applied,
                                  bool mouseWantsVsGrantedMatch = true) {
    std::map<std::uint8_t, std::uint8_t> want;
    for (const auto& d : desired) { want[d.ctrlIdx] = d.type; }
    std::map<std::uint8_t, std::uint8_t> have;
    for (const auto& a : applied) {
        if (a.active) { have[a.ctrlIdx] = a.appliedType; }
    }
    return want == have && mouseWantsVsGrantedMatch;
}

// ── Late-slot converge ──────────────────────────────────────────────────────
// A session PUT snapshots the desired descriptors at send time; slots can
// change between the snapshot and the response landing. This diff returns the
// per-controller follow-ups to converge the live session WITHOUT re-PUTting the
// whole thing: `resyncs` are ctrlIdx whose descriptor changed (or is new) and
// must be re-PUT via the per-controller route; `removes` are ctrlIdx that were
// sent but no longer desired and must be DELETEd.
struct LateConverge {
    std::vector<std::uint8_t> resyncs; // ctrlIdx to PUT /controllers/{idx}
    std::vector<std::uint8_t> removes; // ctrlIdx to DELETE /controllers/{idx}
};

// `sent` and `desired` carry index + type (the fields a descriptor change is
// keyed on for this comparison). An entry present in `desired` but absent from
// `sent`, or present in both with a different type, is a resync; an entry in
// `sent` but not `desired` is a remove.
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

// ── Send-counter exhaustion guard (contract §Crypto) ────────────────────────
// The UDP send counter can never wrap; a session approaching 2^32 self-heals by
// proactively re-PUTting (fresh token/salt/key, counter back to 1). Clients
// SHOULD re-PUT once the send counter crosses 0xF0000000.
inline constexpr std::uint32_t kCounterRepushThreshold = 0xF0000000u;

inline bool counterNeedsRepush(std::uint32_t sendCounter) {
    return sendCounter >= kCounterRepushThreshold;
}

} // namespace dish::reducer
