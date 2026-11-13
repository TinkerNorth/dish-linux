// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The connection-level half of the extended-buttons claim (protocol 4): it
// rides the attach facts like every other capability, defaults to absent, and
// lands in the descriptor as the `extendedButtons` cap.

#include "Network/WifiConnection.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonObject>
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

const QString kSlot = QStringLiteral("direct:steam");

} // namespace

TEST_CASE("the extended-buttons claim rides the attach facts and defaults to absent",
          "[wifi][descriptor]") {
    WifiConnection conn(QStringLiteral("mid:m1"), server());
    conn.attachSlot(kSlot, proto::kControllerTypeXbox, false, false, false);
    auto d = conn.descriptorFor(kSlot);
    REQUIRE(d.has_value());
    CHECK((d->caps & proto::kCapExtendedButtons) == 0);
    CHECK_FALSE(d->toJson().value("caps").toObject().value("extendedButtons").toBool());

    conn.attachSlot(kSlot, proto::kControllerTypeXbox, false, false, false, proto::kTouchpadModeOff,
                    false, false, false, false, false,
                    /*hasExtendedButtons=*/true);
    d = conn.descriptorFor(kSlot);
    REQUIRE(d.has_value());
    CHECK((d->caps & proto::kCapExtendedButtons) != 0);
    CHECK(d->toJson().value("caps").toObject().value("extendedButtons").toBool());

    // A re-attach that drops the fact converges the descriptor, like every
    // other capability fact.
    conn.attachSlot(kSlot, proto::kControllerTypeXbox, false, false, false, proto::kTouchpadModeOff,
                    false, false, false, false, false,
                    /*hasExtendedButtons=*/false);
    d = conn.descriptorFor(kSlot);
    REQUIRE(d.has_value());
    CHECK((d->caps & proto::kCapExtendedButtons) == 0);
}
