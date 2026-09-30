// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The flight recorder behind the Diagnostics page: consecutive snapshots of the
// satellite links and the controller slots, turned into what changed between
// them, so "it dropped some time last night" arrives as a timestamped sequence.
// Onsets only: a flag that stays set logs once, and one that clears logs
// nothing. The first snapshot doubles as the session's opening inventory.

#pragma once

#include "core/reducer/DirectClaimFailure.h"
#include "core/reducer/SatelliteLinkState.h"
#include "core/reducer/UsbPathMachine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dish::reducer {

struct LinkSnapshot {
    std::string id;
    std::string label;
    UiLinkState state = UiLinkState::Saved;

    bool operator==(const LinkSnapshot&) const = default;
};

enum class PadPath : std::uint8_t { UsbStandard, UsbDirect, Bluetooth };

struct PadSnapshot {
    std::string id;
    std::string name;
    PadPath path = PadPath::UsbStandard;
    UsbPhase phase = UsbPhase::Routed;
    std::optional<DirectClaimFailure> failure;
    // The host the pad is bound to: the id says whether it moved, the label is
    // what the log shows. Both empty while it is unbound.
    std::string boundId;
    std::string boundLabel;

    bool operator==(const PadSnapshot&) const = default;
};

enum class DiagnosticsEventKind : std::uint8_t {
    LinkAppeared,
    LinkChanged,
    LinkRemoved,
    PadAttached,
    PadDetached,
    PadNeedsReplug,
    PadRestoreStuck,
    PadDirectFailed,
    PadBound,
    PadUnbound,
};

// One line of the log. The fields after `subject` mean something only for the
// kinds that name them: `from` and `to` for a link (one that appears has only a
// `to`), `path` for an attach, `failure` for a failed claim and `host` for a
// bind or an unbind.
struct DiagnosticsEvent {
    std::int64_t atMs = 0;
    DiagnosticsEventKind kind = DiagnosticsEventKind::LinkAppeared;
    std::string subject;
    UiLinkState from = UiLinkState::Saved;
    UiLinkState to = UiLinkState::Saved;
    PadPath path = PadPath::UsbStandard;
    DirectClaimFailure failure = DirectClaimFailure::Busy;
    std::string host;

    bool operator==(const DiagnosticsEvent&) const = default;
};

// About a day of an ordinary session's churn, and small enough to copy whole
// into a bug report.
inline constexpr std::size_t kDiagnosticsLogCapacity = 200;

namespace detail {

template <class Snapshot>
const Snapshot* findById(const std::vector<Snapshot>& snapshots, const std::string& id) {
    const auto it = std::find_if(snapshots.begin(), snapshots.end(),
                                 [&id](const Snapshot& s) { return s.id == id; });
    return it == snapshots.end() ? nullptr : &*it;
}

inline DiagnosticsEvent linkEvent(DiagnosticsEventKind kind, const LinkSnapshot& link,
                                  std::int64_t atMs) {
    DiagnosticsEvent e;
    e.atMs = atMs;
    e.kind = kind;
    e.subject = link.label;
    e.to = link.state;
    return e;
}

inline DiagnosticsEvent padEvent(DiagnosticsEventKind kind, const PadSnapshot& pad,
                                 std::int64_t atMs) {
    DiagnosticsEvent e;
    e.atMs = atMs;
    e.kind = kind;
    e.subject = pad.name;
    e.path = pad.path;
    e.host = pad.boundLabel;
    return e;
}

inline void appendLinkChange(const LinkSnapshot& before, const LinkSnapshot& after,
                             std::int64_t atMs, std::vector<DiagnosticsEvent>& out) {
    if (before.state == after.state) { return; }
    DiagnosticsEvent changed = linkEvent(DiagnosticsEventKind::LinkChanged, after, atMs);
    changed.from = before.state;
    out.push_back(changed);
}

inline void appendPathOnsets(const PadSnapshot& before, const PadSnapshot& after, std::int64_t atMs,
                             std::vector<DiagnosticsEvent>& out) {
    const bool nowNeedsReplug = after.phase == UsbPhase::NeedsReplug;
    const bool nowStuck = after.phase == UsbPhase::RestoreStuck;
    const bool claimNewlyFailed = !before.failure.has_value() && after.failure.has_value();
    if (nowNeedsReplug && before.phase != UsbPhase::NeedsReplug) {
        out.push_back(padEvent(DiagnosticsEventKind::PadNeedsReplug, after, atMs));
    }
    if (nowStuck && before.phase != UsbPhase::RestoreStuck) {
        out.push_back(padEvent(DiagnosticsEventKind::PadRestoreStuck, after, atMs));
    }
    if (claimNewlyFailed) {
        DiagnosticsEvent failed = padEvent(DiagnosticsEventKind::PadDirectFailed, after, atMs);
        failed.failure = *after.failure;
        out.push_back(failed);
    }
}

// A move to another host is a bind to the new one; only losing the host
// altogether is an unbind, which names the host the pad left.
inline void appendBindingChange(const PadSnapshot& before, const PadSnapshot& after,
                                std::int64_t atMs, std::vector<DiagnosticsEvent>& out) {
    const bool hostChanged = before.boundId != after.boundId;
    if (!hostChanged) { return; }
    const bool nowBound = !after.boundId.empty();
    if (nowBound) {
        out.push_back(padEvent(DiagnosticsEventKind::PadBound, after, atMs));
    } else {
        out.push_back(padEvent(DiagnosticsEventKind::PadUnbound, before, atMs));
    }
}

} // namespace detail

inline std::vector<DiagnosticsEvent> linkEvents(const std::vector<LinkSnapshot>& before,
                                                const std::vector<LinkSnapshot>& after,
                                                std::int64_t atMs) {
    std::vector<DiagnosticsEvent> out;
    for (const auto& link : after) {
        const LinkSnapshot* previous = detail::findById(before, link.id);
        if (previous == nullptr) {
            out.push_back(detail::linkEvent(DiagnosticsEventKind::LinkAppeared, link, atMs));
        } else {
            detail::appendLinkChange(*previous, link, atMs, out);
        }
    }
    for (const auto& link : before) {
        if (detail::findById(after, link.id) == nullptr) {
            out.push_back(detail::linkEvent(DiagnosticsEventKind::LinkRemoved, link, atMs));
        }
    }
    return out;
}

inline std::vector<DiagnosticsEvent> padEvents(const std::vector<PadSnapshot>& before,
                                               const std::vector<PadSnapshot>& after,
                                               std::int64_t atMs) {
    std::vector<DiagnosticsEvent> out;
    for (const auto& pad : after) {
        const PadSnapshot* previous = detail::findById(before, pad.id);
        if (previous == nullptr) {
            out.push_back(detail::padEvent(DiagnosticsEventKind::PadAttached, pad, atMs));
            continue;
        }
        detail::appendPathOnsets(*previous, pad, atMs, out);
        detail::appendBindingChange(*previous, pad, atMs, out);
    }
    for (const auto& pad : before) {
        if (detail::findById(after, pad.id) == nullptr) {
            out.push_back(detail::padEvent(DiagnosticsEventKind::PadDetached, pad, atMs));
        }
    }
    return out;
}

// The log with `events` appended, keeping only the newest `capacity`.
inline std::vector<DiagnosticsEvent> appendBounded(const std::vector<DiagnosticsEvent>& log,
                                                   const std::vector<DiagnosticsEvent>& events,
                                                   std::size_t capacity) {
    std::vector<DiagnosticsEvent> next = log;
    next.insert(next.end(), events.begin(), events.end());
    const bool overflowing = next.size() > capacity;
    if (overflowing) {
        const auto excess = static_cast<std::ptrdiff_t>(next.size() - capacity);
        next.erase(next.begin(), next.begin() + excess);
    }
    return next;
}

} // namespace dish::reducer
