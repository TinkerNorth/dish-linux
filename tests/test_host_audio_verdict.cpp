// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The host controller-audio fold (core/reducer/HostAudioVerdict.h): a present
// `controllerAudio` block wins per direction with `enabled` re-ANDed, an absent
// one falls back to the per-backend `audio` flag, and a satellite predating
// both reads as no audio — which is the truth there. Includes the JSON edge
// cases through the CapabilitiesDto parse, because absence-versus-false is the
// whole trick of this document.

#include "Models/Models.h"
#include "core/reducer/HostAudioVerdict.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>
#include <QJsonObject>

using dish::models::CapabilitiesDto;
using dish::reducer::HostAudioFacts;
using dish::reducer::hostAudioFactsFrom;
using dish::reducer::resolveHostControllerAudio;

namespace {

CapabilitiesDto parse(const char* json) {
    const auto doc = QJsonDocument::fromJson(QByteArray(json));
    REQUIRE(doc.isObject());
    return CapabilitiesDto::fromJson(doc.object());
}

} // namespace

TEST_CASE("a present block answers per direction", "[audio][hostverdict]") {
    HostAudioFacts f;
    f.blockPresent = true;
    f.enabled = true;
    f.mic = true;
    f.speaker = false;
    const auto v = resolveHostControllerAudio(f);
    CHECK(v.mic);
    CHECK_FALSE(v.speaker);

    f.mic = false;
    f.speaker = true;
    const auto flipped = resolveHostControllerAudio(f);
    CHECK_FALSE(flipped.mic);
    CHECK(flipped.speaker);
}

TEST_CASE("enabled=false overrides both directions however they read", "[audio][hostverdict]") {
    // The host folds `enabled` in server-side, but a stale direction switch
    // left true under a disabled master must not advertise an endpoint that
    // will never be plugged: the client re-ANDs rather than trusts.
    HostAudioFacts f;
    f.blockPresent = true;
    f.enabled = false;
    f.mic = true;
    f.speaker = true;
    f.anyBackendAudio = true; // and the fallback must not leak past the block
    const auto v = resolveHostControllerAudio(f);
    CHECK_FALSE(v.mic);
    CHECK_FALSE(v.speaker);
}

TEST_CASE("a present all-false block beats a positive backend flag", "[audio][hostverdict]") {
    // A block that says off IS the verdict; the per-backend flag is only the
    // fallback for satellites predating the block.
    HostAudioFacts f;
    f.blockPresent = true;
    f.anyBackendAudio = true;
    const auto v = resolveHostControllerAudio(f);
    CHECK_FALSE(v.mic);
    CHECK_FALSE(v.speaker);
}

TEST_CASE("an absent block falls back to the backend flag, both directions at once",
          "[audio][hostverdict]") {
    HostAudioFacts f;
    f.blockPresent = false;
    f.anyBackendAudio = true;
    const auto v = resolveHostControllerAudio(f);
    CHECK(v.mic);
    CHECK(v.speaker);

    f.anyBackendAudio = false;
    const auto off = resolveHostControllerAudio(f);
    CHECK_FALSE(off.mic);
    CHECK_FALSE(off.speaker);
}

TEST_CASE("the backend fallback needs available AND audio on one entry", "[audio][hostverdict]") {
    CapabilitiesDto caps;
    dish::models::CapabilitiesBackendDto vigem;
    vigem.id = QStringLiteral("vigem");
    vigem.available = true;
    vigem.audio = false;
    dish::models::CapabilitiesBackendDto maestroDown;
    maestroDown.id = QStringLiteral("hidmaestro");
    maestroDown.available = false; // an unavailable backend materializes nothing
    maestroDown.audio = true;
    caps.backends = {vigem, maestroDown};
    CHECK_FALSE(hostAudioFactsFrom(caps).anyBackendAudio);

    dish::models::CapabilitiesBackendDto maestroUp = maestroDown;
    maestroUp.available = true;
    caps.backends = {vigem, maestroUp};
    CHECK(hostAudioFactsFrom(caps).anyBackendAudio);
    const auto v = resolveHostControllerAudio(caps);
    CHECK(v.mic);
    CHECK(v.speaker);
}

// ── Through the JSON parse ───────────────────────────────────────────────────

TEST_CASE("an absent controllerAudio block parses as unknown, not off", "[audio][hostverdict]") {
    const auto caps = parse(R"({
        "protocolVersion": 2,
        "backends": [
            {"id": "hidmaestro", "supported": true, "available": true, "audio": true}
        ]
    })");
    CHECK_FALSE(caps.hasControllerAudioBlock);
    // The fallback carries it: an older satellite that still speaks audio.
    const auto v = resolveHostControllerAudio(caps);
    CHECK(v.mic);
    CHECK(v.speaker);
}

TEST_CASE("a satellite predating both mechanisms reads as no audio", "[audio][hostverdict]") {
    const auto caps = parse(R"({"protocolVersion": 1, "serverVersion": "0.9.0"})");
    CHECK_FALSE(caps.hasControllerAudioBlock);
    CHECK(caps.backends.isEmpty());
    const auto v = resolveHostControllerAudio(caps);
    CHECK_FALSE(v.mic);
    CHECK_FALSE(v.speaker);
}

TEST_CASE("a partial block reads missing fields as false", "[audio][hostverdict]") {
    // The opt-out the rest of the document takes: a host that stopped sending
    // a field is a host we no longer understand, and offering a microphone it
    // will not plug costs the user a prompt for nothing.
    const auto caps = parse(R"({"controllerAudio": {"enabled": true}})");
    CHECK(caps.hasControllerAudioBlock);
    CHECK(caps.controllerAudioEnabled);
    CHECK_FALSE(caps.controllerAudioMic);
    CHECK_FALSE(caps.controllerAudioSpeaker);
    const auto v = resolveHostControllerAudio(caps);
    CHECK_FALSE(v.mic);
    CHECK_FALSE(v.speaker);
}

TEST_CASE("the full block parses and folds per direction", "[audio][hostverdict]") {
    const auto caps = parse(R"({
        "controllerAudio": {"enabled": true, "mic": true, "speaker": false},
        "backends": [
            {"id": "hidmaestro", "supported": true, "available": true, "audio": true}
        ]
    })");
    CHECK(caps.hasControllerAudioBlock);
    const auto v = resolveHostControllerAudio(caps);
    CHECK(v.mic);
    CHECK_FALSE(v.speaker); // the block wins over the coarser backend flag
}

TEST_CASE("backend entries parse audio absent as false and skip junk", "[audio][hostverdict]") {
    const auto caps = parse(R"({
        "backends": [
            {"id": "vigem", "supported": true, "available": true},
            "not-an-object",
            {"id": "hidmaestro", "supported": true, "available": true, "audio": true}
        ]
    })");
    REQUIRE(caps.backends.size() == 2);
    CHECK(caps.backends[0].id == QStringLiteral("vigem"));
    CHECK_FALSE(caps.backends[0].audio);
    CHECK(caps.backends[1].audio);
}
