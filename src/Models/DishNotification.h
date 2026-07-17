// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Dish replacement for a single-shot QLabel "error banner" — a brand-styled,
// stacked, optionally-actionable toast. Field shape mirrors dish-android's
// `DishNotification` (app/src/main/java/.../core/model/DishNotification.kt) so
// the two clients share one mental model and one wording catalogue.
//
// Pure value type: this header has no QObject — it lives in `Models/` because
// it has no flow shape of its own. The publisher is `dish::ui::NotificationQueue`
// in `UI/NotificationQueue.h`; that class is the bus that owns IDs, dedup and
// dismissal.

#pragma once

#include <QMetaType>
#include <QString>

#include <cstdint>

namespace dish::models {

// Visual severity rail + sensible-default duration. ERROR/WARN auto-dismiss
// the same as INFO/SUCCESS by default — *persistent* banners are explicit
// opt-in via [DishNotification::Duration::Persistent] (mirrors the dish-android
// PR that fixed transient ERRORs sticking around forever).
enum class NotificationSeverity : std::uint8_t { Info, Success, Warn, Error };

// Source axis: a hint for the renderer (icon family + a11y label) and for
// future filtering. Today everything is rendered the same way; the field is
// here so the UI can grow per-kind glyphs without a schema change.
enum class NotificationKind : std::uint8_t {
    Generic,
    Connection, // satellite session / pairing
    Controller, // SDL pad attach/detach, binding
    System,     // host battery, network, etc.
};

// One queued/visible toast. Constructed by `NotificationQueue::post(...)` —
// the queue assigns the monotonic id so call sites don't have to thread one
// in. `dismissible == false` opts the toast out of click-to-dismiss; the
// auto-dismiss timer still applies unless `durationMs == kPersistent`.
struct DishNotification {
    // Stay-until-dismissed sentinel. Matches dish-android's
    // DishNotification.DURATION_PERSISTENT (0 ms).
    static constexpr int kPersistent = 0;
    // Roughly Toast.LENGTH_SHORT — INFO/SUCCESS default.
    static constexpr int kDurationShortMs = 3'500;
    // Roughly Toast.LENGTH_LONG — WARN/ERROR default.
    static constexpr int kDurationLongMs = 6'000;

    std::uint64_t id = 0;
    NotificationKind kind = NotificationKind::Generic;
    NotificationSeverity severity = NotificationSeverity::Info;
    QString message;
    bool dismissible = true;
    int durationMs = kDurationShortMs;
};

// Severity-default duration. The renderer / queue uses this when the call
// site doesn't pass an explicit `durationMs`.
inline int defaultDurationFor(NotificationSeverity severity) {
    switch (severity) {
    case NotificationSeverity::Info:
    case NotificationSeverity::Success:
        return DishNotification::kDurationShortMs;
    case NotificationSeverity::Warn:
    case NotificationSeverity::Error:
        return DishNotification::kDurationLongMs;
    }
    return DishNotification::kDurationShortMs;
}

} // namespace dish::models

// Allow DishNotification to ride QSignalSpy / queued connections by value —
// it's a plain POD with QString fields, so the default-generated metatype is
// fine.
Q_DECLARE_METATYPE(dish::models::DishNotification)
