// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Descriptor-aware late-slot converge. A session PUT snapshots the desired
// descriptors at send time, and slots can change before the response lands; this
// diff returns the per-controller follow-ups that converge the live session
// without re-PUTting the whole thing. Stricter than the (ctrlIdx, type) variant
// in Reconcile.h: a touchpad-mode flip or a caps change at the same index is a
// resync too.

#pragma once

#include "core/model/Protocol.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace dish::reducer {

// Exactly the fields the per-controller PUT body carries, so any one of them
// differing means the server's applied descriptor is stale.
struct DescriptorSlot {
    std::uint8_t ctrlIdx = 0;
    std::uint8_t type = proto::kControllerTypeXbox;
    std::uint16_t caps = 0;
    std::uint8_t touchpadMode = proto::kTouchpadModeOff;

    // Excludes ctrlIdx: the index is the map key, not part of the comparison.
    bool sameDescriptorAs(const DescriptorSlot& o) const {
        return type == o.type && caps == o.caps && touchpadMode == o.touchpadMode;
    }
};

// `resyncs` must be re-PUT via PUT /controllers/{idx}, `deletes` DELETEd. Both
// ascending, so the order is deterministic for tests and replay.
struct LateConvergeDesc {
    std::vector<std::uint8_t> resyncs;
    std::vector<std::uint8_t> deletes;

    bool operator==(const LateConvergeDesc& o) const {
        return resyncs == o.resyncs && deletes == o.deletes;
    }
};

// An index re-used with an identical descriptor is left alone.
inline LateConvergeDesc lateSlotConvergeDesc(const std::vector<DescriptorSlot>& sent,
                                             const std::vector<DescriptorSlot>& desired) {
    LateConvergeDesc out;
    std::map<std::uint8_t, DescriptorSlot> sentByIdx;
    for (const auto& s : sent) { sentByIdx[s.ctrlIdx] = s; }
    std::map<std::uint8_t, DescriptorSlot> desiredByIdx;
    for (const auto& d : desired) { desiredByIdx[d.ctrlIdx] = d; }

    for (const auto& [idx, d] : desiredByIdx) {
        const auto it = sentByIdx.find(idx);
        if (it == sentByIdx.end() || !it->second.sameDescriptorAs(d)) {
            out.resyncs.push_back(idx);
        }
    }
    for (const auto& [idx, s] : sentByIdx) {
        if (desiredByIdx.find(idx) == desiredByIdx.end()) { out.deletes.push_back(idx); }
    }
    std::sort(out.resyncs.begin(), out.resyncs.end());
    std::sort(out.deletes.begin(), out.deletes.end());
    return out;
}

} // namespace dish::reducer
