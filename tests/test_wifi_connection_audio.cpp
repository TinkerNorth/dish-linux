// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The connection-level half of controller audio: the descriptor's audio caps
// ride the attach facts (and default to ABSENT, which is the whole Wave-1
// posture), and the host's probed verdict is per-session state that resets with
// the session it was probed for.

#include "Network/WifiConnection.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::net::WifiConnection;
namespace proto = dish::proto;

namespace {

dish::models::DiscoveredServer server() {
    dish::models::DiscoveredServer s;
    s.machineId = QStringLiteral("m1");
    s.ip = QStringLiteral("127.0.0.1");
    s.name = QStringLiteral("Sat");
    return s;
}

} // namespace

TEST_CASE("an attach without audio facts advertises neither audio cap", "[wifi][audio]") {
    // The Wave-1 end-to-end invariant, at the wire's edge: every caller of
    // attachSlot folds the audio route seam, which answers false for every
    // slot, so the PUT body must carry mic:false, speaker:false everywhere.
    WifiConnection conn(QStringLiteral("mid:m1"), server());
    conn.attachSlot(QStringLiteral("sdl:1"), proto::kControllerTypeDualSense,
                    /*hasLightbar=*/true, /*hasMotion=*/true, /*hasRumble=*/true,
                    proto::kTouchpadModeDs4, /*hasTriggerEffects=*/true, /*hasPlayerLeds=*/true);
    const auto d = conn.descriptorFor(QStringLiteral("sdl:1"));
    REQUIRE(d.has_value());
    CHECK((d->caps & proto::kCapMic) == 0);
    CHECK((d->caps & proto::kCapSpeaker) == 0);
    // The rest of the fold is untouched by the audio additions.
    CHECK((d->caps & proto::kCapTriggerEffects) != 0);
    CHECK((d->caps & proto::kCapRumble) != 0);
}

TEST_CASE("the attach facts fold each audio cap independently", "[wifi][audio]") {
    WifiConnection conn(QStringLiteral("mid:m1"), server());
    conn.attachSlot(QStringLiteral("sdl:1"), proto::kControllerTypeDualSense,
                    /*hasLightbar=*/false, /*hasMotion=*/false, /*hasRumble=*/false,
                    proto::kTouchpadModeOff, /*hasTriggerEffects=*/false, /*hasPlayerLeds=*/false,
                    /*hasMic=*/true, /*hasSpeaker=*/false);
    auto d = conn.descriptorFor(QStringLiteral("sdl:1"));
    REQUIRE(d.has_value());
    CHECK((d->caps & proto::kCapMic) != 0);
    CHECK((d->caps & proto::kCapSpeaker) == 0);

    // A re-attach with the toggles moved converges the descriptor, same as
    // every other capability fact.
    conn.attachSlot(QStringLiteral("sdl:1"), proto::kControllerTypeDualSense, false, false, false,
                    proto::kTouchpadModeOff, false, false, /*hasMic=*/false, /*hasSpeaker=*/true);
    d = conn.descriptorFor(QStringLiteral("sdl:1"));
    REQUIRE(d.has_value());
    CHECK((d->caps & proto::kCapMic) == 0);
    CHECK((d->caps & proto::kCapSpeaker) != 0);
}

TEST_CASE("the host audio verdict defaults conservative and resets with the session",
          "[wifi][audio]") {
    WifiConnection conn(QStringLiteral("mid:m1"), server());
    // No probe has landed: no audio, in either direction.
    CHECK_FALSE(conn.hostMicAvailable());
    CHECK_FALSE(conn.hostSpeakerAvailable());

    conn.setHostControllerAudio(/*mic=*/true, /*speaker=*/true);
    CHECK(conn.hostMicAvailable());
    CHECK(conn.hostSpeakerAvailable());

    // The directions move independently, as the host switches them.
    conn.setHostControllerAudio(/*mic=*/false, /*speaker=*/true);
    CHECK_FALSE(conn.hostMicAvailable());
    CHECK(conn.hostSpeakerAvailable());

    // A teardown is a new session next time, and the verdict was THIS
    // session's: it must not survive into one the host may have re-switched.
    // (markConnecting first: an Idle connection with no client short-circuits
    // markDisconnected, and only a session that existed can be torn down.)
    conn.markConnecting();
    conn.markDisconnected();
    CHECK_FALSE(conn.hostMicAvailable());
    CHECK_FALSE(conn.hostSpeakerAvailable());
}
