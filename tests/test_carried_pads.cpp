// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The connection→pads join behind the Forget manifest and the "slot 2 of 4"
// ordinal. Both read the same list in the same order, so the manifest and the
// ordinal beside it can never disagree.

#include "core/reducer/CarriedPads.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::reducer::carriedPads;
using dish::reducer::slotOrdinalOnConnection;
namespace m = dish::models;

namespace {

m::ControllerSlot slot(const QString& id, const QString& name, const QString& boundTo,
                       const QString& emulateName = {}) {
    m::ControllerSlot s;
    s.id = id;
    s.name = name;
    if (!boundTo.isEmpty()) { s.boundConnectionId = boundTo; }
    s.emulateName = emulateName;
    return s;
}

QList<m::ControllerSlot> fixture() {
    return {
        slot(QStringLiteral("s1"), QStringLiteral("DualSense"), QStringLiteral("conn-a"),
             QStringLiteral("DualShock 4")),
        slot(QStringLiteral("s2"), QStringLiteral("Xbox Pad"), QStringLiteral("conn-b")),
        slot(QStringLiteral("s3"), QStringLiteral("Switch Pro"), QStringLiteral("conn-a")),
        slot(QStringLiteral("s4"), QStringLiteral("8BitDo"), QString()),
    };
}

} // namespace

TEST_CASE("carriedPads returns only the pads on that connection, in list order", "[carried-pads]") {
    const auto pads = carriedPads(fixture(), QStringLiteral("conn-a"));
    REQUIRE(pads.size() == 2);
    CHECK(pads[0].name == QStringLiteral("DualSense"));
    CHECK(pads[0].emulateName == QStringLiteral("DualShock 4"));
    CHECK(pads[1].name == QStringLiteral("Switch Pro"));
    // No catalog name for that type: the manifest prints the pad alone.
    CHECK(pads[1].emulateName.isEmpty());
}

TEST_CASE("carriedPads ignores unbound pads and unknown connections", "[carried-pads]") {
    CHECK(carriedPads(fixture(), QStringLiteral("conn-zzz")).isEmpty());
    CHECK(carriedPads({}, QStringLiteral("conn-a")).isEmpty());
    // An empty id is the no-selection case, not a match against unbound pads.
    CHECK(carriedPads(fixture(), QString()).isEmpty());
}

TEST_CASE("slotOrdinal counts only within the same connection", "[carried-pads]") {
    const auto slotList = fixture();
    CHECK(slotOrdinalOnConnection(slotList, QStringLiteral("s1"), QStringLiteral("conn-a")) == 1);
    // s2 rides another host, so s3 is the SECOND pad on conn-a, not the third.
    CHECK(slotOrdinalOnConnection(slotList, QStringLiteral("s3"), QStringLiteral("conn-a")) == 2);
    CHECK(slotOrdinalOnConnection(slotList, QStringLiteral("s2"), QStringLiteral("conn-b")) == 1);
}

TEST_CASE("slotOrdinal is 0 when the pad does not ride that connection", "[carried-pads]") {
    const auto slotList = fixture();
    CHECK(slotOrdinalOnConnection(slotList, QStringLiteral("s1"), QStringLiteral("conn-b")) == 0);
    CHECK(slotOrdinalOnConnection(slotList, QStringLiteral("s4"), QStringLiteral("conn-a")) == 0);
    CHECK(slotOrdinalOnConnection(slotList, QStringLiteral("s1"), QString()) == 0);
    CHECK(slotOrdinalOnConnection({}, QStringLiteral("s1"), QStringLiteral("conn-a")) == 0);
}

TEST_CASE("the ordinal agrees with the manifest it renders beside", "[carried-pads]") {
    const auto slotList = fixture();
    const auto pads = carriedPads(slotList, QStringLiteral("conn-a"));
    for (int i = 0; i < pads.size(); ++i) {
        // The pad at manifest position i is the one whose ordinal is i + 1.
        const QString id = (i == 0) ? QStringLiteral("s1") : QStringLiteral("s3");
        CHECK(slotOrdinalOnConnection(slotList, id, QStringLiteral("conn-a")) == i + 1);
    }
}
