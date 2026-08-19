// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Folds a slot's motion facts down to one user-facing state for the gyro chip.
// The most blocking reason wins, so the user sees a single actionable
// explanation rather than a pile of overlapping warnings. Returns an enum; the UI
// maps it to a localized label and glyph.

#pragma once

namespace dish::reducer {

// Declaration order is the precedence order; do not reorder without updating
// motionIndicatorFor and the test table.
enum class MotionIndicatorState {
    Unavailable,   // no gyro hardware
    UserDisabled,  // user turned motion off
    NotForwarded,  // not on a live link that carries motion
    NoHostSink,    // the satellite only sinks motion for PlayStation-typed slots
    BackendBroken, // host sink present but its motion backend is unhealthy
    Stalled,       // wired and live but no samples arriving
    Streaming,
    Paused, // wired and live, intentionally paused
};

struct MotionIndicatorInputs {
    bool hasGyro = false;
    bool userEnabled = true;
    bool carriesOnConnection = false;
    bool hostHasSinkForType = true;
    bool backendOk = true; // only consulted once hostHasSinkForType holds
    bool isStreaming = false;
    bool isPaused = false;
};

inline MotionIndicatorState motionIndicatorFor(const MotionIndicatorInputs& in) {
    if (!in.hasGyro) { return MotionIndicatorState::Unavailable; }
    if (!in.userEnabled) { return MotionIndicatorState::UserDisabled; }
    if (!in.carriesOnConnection) { return MotionIndicatorState::NotForwarded; }
    if (!in.hostHasSinkForType) { return MotionIndicatorState::NoHostSink; }
    if (!in.backendOk) { return MotionIndicatorState::BackendBroken; }
    if (in.isStreaming) { return MotionIndicatorState::Streaming; }
    if (in.isPaused) { return MotionIndicatorState::Paused; }
    // Wired and live but neither streaming nor paused: report the quiet gyro as a
    // stall so the user knows samples are not landing.
    return MotionIndicatorState::Stalled;
}

// ── Meter-visibility conjunctions ────────────────────────────────────────────

// Every state other than Unavailable still shows the meter, so the user can see
// it sitting at 0 Hz alongside the reason.
inline bool motionRateUserFacingOn(bool hasMotionCapability, MotionIndicatorState state) {
    return hasMotionCapability && state != MotionIndicatorState::Unavailable;
}

// A real pad's touchpad rate is only meaningful with the link live and touchpad
// forwarding on.
inline bool screenRateUserFacingOn(bool satelliteConnected, bool touchpadForwardingOn) {
    return satelliteConnected && touchpadForwardingOn;
}

} // namespace dish::reducer
