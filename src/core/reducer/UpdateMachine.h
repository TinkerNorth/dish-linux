// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The update checker's lifecycle FSM. Same shape as UsbPathMachine: every
// (phase x event) pair is total, `reduceUpdate` does no IO and reads no clock,
// and effects come back as data for update/UpdateChecker to execute against
// the network and the timers.
//
// Dish never installs its own update: a distro package or Flatpak owns the
// binary. So the machine stops at Available and the UI links out — there is no
// download, no staging and no apply arm to get wrong.
//
// One rule earns its own comment because getting it wrong ships a client that
// nags forever or never: ordering is by the parsed version triple ONLY.
// `publishedAt` is display text and no wall clock enters this file, so a
// machine with a skewed clock behaves exactly like one without.
//
// The scheduling constants live here rather than in core/reducer/Backoff.h on
// purpose: that ladder is the 1 s..60 s satellite reconnect scale, and polling
// a release host on it would be abuse.

#pragma once

#include "core/update/UpdateManifest.h"

#include <QString>

#include <variant>
#include <vector>

namespace dish::reducer {

enum class UpdatePhase {
    Disabled,  // checks turned off: no timers, no QNAM, no network IO at all
    Idle,      // enabled, nothing known yet
    Checking,  // a manifest fetch is in flight
    UpToDate,  // the newest published release is this one (or a skipped one)
    Available, // newer exists; the UI links to it
    Failed,    // the last attempt failed; `error` says how
};

enum class UpdateError {
    None,
    Offline,         // reachability gate; no request was made
    Http,            // transport, TLS, status code
    ManifestInvalid, // any UpdateManifest::parse rejection, incl. portal HTML
};

// The checker turns these into localized toasts; the engine never vends a
// sentence.
enum class UpdateNotice { Available, Unsupported };

enum class UpdateTrigger { Startup, Periodic, Manual, Retry };

// ==-comparable so the checker's Observable suppresses no-op re-emits.
struct UpdateStatus {
    UpdatePhase phase = UpdatePhase::Idle;
    QString currentVersion; // DISH_VERSION, seeded once by the checker
    QString availableVersion;
    QString notesUrl;
    QString minimumSupportedVersion;

    int consecutiveFailures = 0;
    UpdateError error = UpdateError::None;

    bool checksEnabled = true;
    bool required = false; // currentVersion < minimumSupportedVersion

    // Beyond the surfaced state, and both are what make `reduceUpdate` total
    // without a second source of truth: `skippedVersion` because a skip must
    // stay muted while the manifest still offers that exact version, and
    // `online` because the gate is evaluated at CheckRequested time, not when
    // the connectivity event arrived.
    QString skippedVersion;
    bool online = true;

    bool operator==(const UpdateStatus& o) const {
        return phase == o.phase && currentVersion == o.currentVersion &&
               availableVersion == o.availableVersion && notesUrl == o.notesUrl &&
               minimumSupportedVersion == o.minimumSupportedVersion &&
               consecutiveFailures == o.consecutiveFailures && error == o.error &&
               checksEnabled == o.checksEnabled && required == o.required &&
               skippedVersion == o.skippedVersion && online == o.online;
    }
    bool operator!=(const UpdateStatus& o) const { return !(*this == o); }
};

// ── Scheduling constants ────────────────────────────────────────────────────
// Startup is delayed so the check never competes with the first frame; the min
// gap keeps a relaunch loop from hammering the permalink, and the future-jump
// escape means a clock that reads 2099 costs one extra check, not silence.

inline constexpr int kStartupDelayMs = 15'000;
inline constexpr qint64 kMinCheckGapMs = 60LL * 60 * 1000;           // 1 h
inline constexpr qint64 kFutureSkewEscapeMs = 24LL * 60 * 60 * 1000; // 24 h
inline constexpr int kPeriodicIntervalMs = 4 * 60 * 60 * 1000;       // 4 h
inline constexpr int kBackoffBaseMs = 10 * 60 * 1000;                // 10 min
inline constexpr int kBackoffCapMs = 6 * 60 * 60 * 1000;             // 6 h
inline constexpr double kBackoffJitter = 0.2;                        // +-20 percent
inline constexpr int kManualMinGapMs = 10'000;                       // manual rate limit
inline constexpr int kReconnectCheckDelayMs = 30'000;                // after Online returns

// 10, 20, 40 ... 360 min. `failures` is the count INCLUDING the one just
// recorded, so the first failure waits kBackoffBaseMs.
constexpr int backoffDelayMs(int failures) {
    if (failures <= 1) { return kBackoffBaseMs; }
    long long delay = kBackoffBaseMs;
    for (int i = 1; i < failures; ++i) {
        delay *= 2;
        if (delay >= kBackoffCapMs) { return kBackoffCapMs; }
    }
    return static_cast<int>(delay);
}

// Deterministic given `unit` in [0, 1]: the checker supplies the random draw,
// so the ladder itself stays pinnable in a test. Applied ONLY to failure
// backoff (spreading a fleet's retries), never to the 15 s startup delay or
// the 4 h interval, both of which are asserted verbatim.
constexpr int jitteredDelayMs(int baseMs, double unit) {
    const double clamped = unit < 0.0 ? 0.0 : (unit > 1.0 ? 1.0 : unit);
    const double factor = 1.0 - kBackoffJitter + (2.0 * kBackoffJitter * clamped);
    const double scaled = static_cast<double>(baseMs) * factor;
    return scaled < 0.0 ? 0 : static_cast<int>(scaled);
}

// ── Events ──────────────────────────────────────────────────────────────────

namespace update_event {

// The reactive preference slice; the checker republishes it on every store
// change, so this event is also how "checks off" reaches every phase.
struct PrefsChanged {
    bool checksEnabled = true;
    QString skippedVersion;
    bool operator==(const PrefsChanged& o) const {
        return checksEnabled == o.checksEnabled && skippedVersion == o.skippedVersion;
    }
};
struct CheckRequested {
    UpdateTrigger trigger = UpdateTrigger::Periodic;
    bool operator==(const CheckRequested& o) const { return trigger == o.trigger; }
};
struct ManifestArrived {
    dish::update::UpdateManifest manifest;
    bool operator==(const ManifestArrived& o) const { return manifest == o.manifest; }
};
struct CheckFailed {
    UpdateError error = UpdateError::Http;
    bool operator==(const CheckFailed& o) const { return error == o.error; }
};
struct SkipRequested {
    QString version;
    bool operator==(const SkipRequested& o) const { return version == o.version; }
};
struct ReachabilityChanged {
    bool online = true;
    bool operator==(const ReachabilityChanged& o) const { return online == o.online; }
};

} // namespace update_event

using UpdateEvent = std::variant<update_event::PrefsChanged, update_event::CheckRequested,
                                 update_event::ManifestArrived, update_event::CheckFailed,
                                 update_event::SkipRequested, update_event::ReachabilityChanged>;

// ── Effects (returned as data; executed by the checker) ─────────────────────

namespace update_effect {

// GET the release permalink. `manual` only relaxes the checker's rate
// limiting; the request itself is identical.
struct FetchManifest {
    bool manual = false;
    bool operator==(const FetchManifest& o) const { return manual == o.manual; }
};
struct ScheduleNextCheck {
    int delayMs = 0;
    bool operator==(const ScheduleNextCheck& o) const { return delayMs == o.delayMs; }
};
struct PersistLastCheck {
    bool operator==(const PersistLastCheck&) const { return true; }
};
struct Notify {
    UpdateNotice notice = UpdateNotice::Available;
    QString version;
    bool operator==(const Notify& o) const { return notice == o.notice && version == o.version; }
};

} // namespace update_effect

using UpdateEffect = std::variant<update_effect::FetchManifest, update_effect::ScheduleNextCheck,
                                  update_effect::PersistLastCheck, update_effect::Notify>;

struct UpdateReduction {
    UpdateStatus next;
    std::vector<UpdateEffect> effects;
};

UpdateReduction reduceUpdate(const UpdateStatus& s, const UpdateEvent& event);

} // namespace dish::reducer
