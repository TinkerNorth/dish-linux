// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The host's controller-audio verdict, folded from GET /api/server/capabilities.
// That one document is the ONLY carrier: not the catalog (cached on server
// version + locale, so a switch flipped on the PC must not move it) and not the
// session PUT. Three layers answer three different questions — a descriptor cap
// says what this CLIENT can do, the catalog's type slugs what the host COULD
// build, and this verdict what the host will actually carry RIGHT NOW. It can
// flip under a live session, so callers keep it as per-connection state with a
// conservative default: no audio until a probe says yes.

#pragma once

#include "Models/Models.h"

namespace dish::reducer {

// The facts the fold reads, lifted out of the DTO so the rule is testable as a
// truth table rather than through JSON.
struct HostAudioFacts {
    // The top-level `controllerAudio` block. ABSENT is unknown, not off: a
    // satellite predating the block may still carry audio and reports it on the
    // per-backend `audio` flag, so reading absence as two falses would mute
    // every host older than the block.
    bool blockPresent = false;
    bool enabled = false;
    bool mic = false;
    bool speaker = false;
    // The fallback: any backends[] entry that is available AND carries audio.
    // A satellite predating BOTH mechanisms sends no backends array either,
    // which reads as off — and is the truth there.
    bool anyBackendAudio = false;
};

struct HostAudioVerdict {
    bool mic = false;
    bool speaker = false;
};

// A PRESENT block wins outright, since it is the only place the two directions
// are reported apart. `enabled` is re-ANDed rather than trusted: the host does
// fold it into both directions, but a stale direction switch left true under a
// disabled master would otherwise advertise an endpoint that will never be
// plugged. An absent block falls back to the per-backend flag, both directions
// at once, because that flag cannot tell them apart.
inline HostAudioVerdict resolveHostControllerAudio(const HostAudioFacts& f) {
    if (f.blockPresent) { return {f.enabled && f.mic, f.enabled && f.speaker}; }
    return {f.anyBackendAudio, f.anyBackendAudio};
}

// The DTO-to-facts bridge, kept next to the rule so no caller re-reads the
// document's fields with its own ideas about absence.
inline HostAudioFacts hostAudioFactsFrom(const models::CapabilitiesDto& caps) {
    HostAudioFacts f;
    f.blockPresent = caps.hasControllerAudioBlock;
    f.enabled = caps.controllerAudioEnabled;
    f.mic = caps.controllerAudioMic;
    f.speaker = caps.controllerAudioSpeaker;
    for (const auto& b : caps.backends) {
        if (b.available && b.audio) {
            f.anyBackendAudio = true;
            break;
        }
    }
    return f;
}

inline HostAudioVerdict resolveHostControllerAudio(const models::CapabilitiesDto& caps) {
    return resolveHostControllerAudio(hostAudioFactsFrom(caps));
}

} // namespace dish::reducer
