// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// RememberedWifi's value-equality, which ConnectionStore uses to decide that a
// row is unchanged and skip the write.
//
// That makes a field left out of the comparison a silent data-loss bug: the
// field changes, the store sees no change, nothing is persisted, and the value
// comes back as it was on the next launch. There is no crash and no log line.
//
// So the comparison is checked against the PERSISTENCE surface rather than
// against a list of fields kept here. Whatever toJson writes is what the next
// launch reads back, so anything toJson carries must also make two rows
// unequal - and a field added to the struct and to toJson is caught without
// this file being touched.

#include "repository/RememberedWifiEq.h"

#include "Models/Models.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

using dish::models::RememberedWifi;

namespace {

RememberedWifi populated() {
    RememberedWifi r;
    r.id = QStringLiteral("wifi-1");
    r.name = QStringLiteral("Living room");
    r.ip = QStringLiteral("192.0.2.7");
    r.udpPort = 41234;
    r.pairPort = 41235;
    r.httpPort = 41236;
    r.machineId = QStringLiteral("4d2c0f1e");
    return r;
}

// A value of the same JSON type that is certainly different from `from`.
QJsonValue otherThan(const QJsonValue& from) {
    if (from.isDouble()) { return QJsonValue(from.toInt() + 1); }
    if (from.isBool()) { return QJsonValue(!from.toBool()); }
    return QJsonValue(from.toString() + QStringLiteral("-changed"));
}

} // namespace

TEST_CASE("remembered wifi: a row equals itself and a copy of itself", "[models][remembered]") {
    const auto a = populated();
    CHECK(a == a);
    CHECK(a == populated());
    CHECK_FALSE(a != populated());
}

TEST_CASE("remembered wifi: every persisted field makes two rows unequal", "[models][remembered]") {
    // Driven off toJson, not off a list of fields written down here: a field
    // added to the struct and to the persisted shape is covered the moment it
    // is added, which is the case this test exists for.
    const auto original = populated();
    const QJsonObject json = original.toJson();
    REQUIRE_FALSE(json.isEmpty());

    for (auto it = json.begin(); it != json.end(); ++it) {
        INFO("persisted field " << it.key().toStdString());
        QJsonObject changed = json;
        changed[it.key()] = otherThan(it.value());
        // Guard against a value that did not actually change.
        REQUIRE(changed.value(it.key()) != it.value());

        const auto other = RememberedWifi::fromJson(changed);
        // If this fails, the field round-trips through storage but not through
        // the comparison, so a change to it is written once and never again.
        CHECK(original != other);
        CHECK_FALSE(original == other);
    }
}

TEST_CASE("remembered wifi: a full round trip through JSON compares equal",
          "[models][remembered]") {
    // The other half of the same claim: nothing in the comparison may read a
    // field that storage does not carry, or a row reloaded from disk would look
    // changed on every launch and be rewritten forever.
    const auto original = populated();
    CHECK(original == RememberedWifi::fromJson(original.toJson()));
}

TEST_CASE("remembered wifi: a default row equals a default row", "[models][remembered]") {
    // The defaults are real values (the three default ports), so two untouched
    // rows must compare equal rather than merely not-crash.
    CHECK(RememberedWifi{} == RememberedWifi{});
    CHECK(RememberedWifi{} == RememberedWifi::fromJson(QJsonObject{}));
}
