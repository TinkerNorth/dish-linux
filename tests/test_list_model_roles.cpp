// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Every role of a QAbstractListModel is written down THREE times: once in the
// Roles enum, once in roleNames() as the name QML binds to, and once in data()
// as the arm that answers it. Two of the three agreeing is enough for the code
// to build, for every other test to pass, and for the property to read
// `undefined` in QML, which binds as false, as an empty string, and as a
// disabled control. Nothing in the language or in Qt checks the third.
//
// So this file checks it, from the metaobject rather than from a list kept
// here, because a list kept here would be a fourth place to forget.
//
// Each model is driven through its real setters with one fully populated row,
// which is what makes "data() answers" mean something: a role served from a
// field that is never set would answer just as well from an empty row.

#include "Models/Models.h"
#include "composer/ConnectionsComposer.h"
#include "core/reducer/DirectClaimFailure.h"
#include "core/reducer/LatencyWindow.h"
#include "core/reducer/PathChoice.h"
#include "core/reducer/UsbPathMachine.h"
#include "qml/ConnectionListModel.h"
#include "qml/SlotListModel.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QHash>
#include <QMetaEnum>
#include <QSet>
#include <QString>
#include <QVariant>

#include <vector>

using dish::qml::ConnectionListModel;
using dish::qml::SlotListModel;
namespace m = dish::models;

namespace {

// The enumerators as the metaobject reports them, so adding one to the enum is
// enough to put it under test.
template <typename Roles> std::vector<std::pair<QByteArray, int>> declaredRoles() {
    const QMetaEnum meta = QMetaEnum::fromType<Roles>();
    std::vector<std::pair<QByteArray, int>> out;
    out.reserve(static_cast<std::size_t>(meta.keyCount()));
    for (int i = 0; i < meta.keyCount(); ++i) {
        out.emplace_back(QByteArray(meta.key(i)), meta.value(i));
    }
    return out;
}

// Names the enumerator rather than the number, because a failure that says
// `HasRumbleRole` is a failure someone can act on and `260` is not.
template <typename Model, typename Roles>
void checkRolesAreWholeAndServed(const Model& model, const QModelIndex& index) {
    const QHash<int, QByteArray> names = model.roleNames();

    QSet<QByteArray> seenNames;
    for (const auto& [key, value] : declaredRoles<Roles>()) {
        INFO("enumerator " << key.constData());
        // Declared but unnamed: QML cannot reach it at all.
        REQUIRE(names.contains(value));

        const QByteArray qmlName = names.value(value);
        INFO("QML property " << qmlName.constData());
        REQUIRE_FALSE(qmlName.isEmpty());
        // Two roles under one name: the later one shadows the earlier, and the
        // shadowed property reads the wrong field forever.
        REQUIRE_FALSE(seenNames.contains(qmlName));
        seenNames.insert(qmlName);

        // Named but unserved: data() falls through to its default and returns an
        // invalid QVariant, which QML reads as undefined.
        REQUIRE(model.data(index, value).isValid());
    }

    // And nothing named that is not declared: a stale entry left behind by a
    // removed role vends a property that answers undefined.
    REQUIRE(names.size() == static_cast<int>(declaredRoles<Roles>().size()));
}

m::ControllerSlot populatedSlot() {
    m::ControllerSlot s;
    s.id = QStringLiteral("slot-1");
    s.name = QStringLiteral("Wireless Controller");
    s.boundConnectionId = QStringLiteral("conn-1");
    m::ConnectionSummary summary;
    summary.id = QStringLiteral("conn-1");
    summary.label = QStringLiteral("Living room");
    summary.live = m::LinkState::Connected;
    s.boundStatus = summary;
    s.usbDirect = true;
    s.bluetooth = false;
    s.remappable = true;
    s.emulateName = QStringLiteral("DualSense");
    s.registering = false;
    s.verifiedModel = true;
    s.micArmed = true;
    s.micMuted = false;
    s.capabilities.hasMotion = true;
    s.capabilities.hasLightbar = true;
    s.capabilities.hasTouchpad = true;
    s.capabilities.hasRumble = true;
    s.capabilities.batteryLevel = 82;
    s.capabilities.batteryStatus = 1;
    s.pathPhase = dish::reducer::UsbPhase::Direct;
    s.desiredPath = dish::reducer::PathChoice::Direct;
    s.pathSupported = true;
    s.directFailure = dish::reducer::DirectClaimFailure::Busy;
    s.liveRates.gamepadHz = 250;
    s.liveRates.motionHz = 200;
    s.liveRates.directPollHz = 1000;
    return s;
}

dish::composer::ConnectionRow populatedRow() {
    dish::composer::ConnectionRow row;
    row.id = "conn-1";
    row.ip = "192.0.2.7";
    row.live = dish::reducer::UiLinkState::Connected;
    row.boundSlotId = "slot-1";
    row.latencyOneWayMs = 3.5;
    row.latencySamples = 64;
    return row;
}

} // namespace

TEST_CASE("every slot role is declared, named uniquely, and answered", "[qml][roles][slots]") {
    SlotListModel model;
    model.setState(QList<m::ControllerSlot>{populatedSlot()});
    model.setConnectionRows({populatedRow()});
    REQUIRE(model.rowCount() == 1);

    checkRolesAreWholeAndServed<SlotListModel, SlotListModel::Roles>(model, model.index(0, 0));
}

TEST_CASE("a slot with no satellite row still answers every role", "[qml][roles][slots]") {
    // The Sat* roles join in a row that may not be there. They must read empty
    // rather than invalid, or an unbound slot disables its own controls.
    SlotListModel model;
    auto slot = populatedSlot();
    slot.boundConnectionId.reset();
    slot.boundStatus.reset();
    slot.directFailure.reset();
    model.setState(QList<m::ControllerSlot>{slot});
    REQUIRE(model.rowCount() == 1);

    checkRolesAreWholeAndServed<SlotListModel, SlotListModel::Roles>(model, model.index(0, 0));
}

TEST_CASE("every connection role is declared, named uniquely, and answered",
          "[qml][roles][connections]") {
    ConnectionListModel model;
    model.setRows({populatedRow()});
    REQUIRE(model.rowCount() == 1);

    checkRolesAreWholeAndServed<ConnectionListModel, ConnectionListModel::Roles>(model,
                                                                                 model.index(0, 0));
}

TEST_CASE("an out-of-range index answers nothing at all", "[qml][roles]") {
    // The counterpart of the rule above: invalid is exactly how "there is no
    // such row" is said, so it must not be reachable any other way.
    SlotListModel model;
    model.setState(QList<m::ControllerSlot>{populatedSlot()});
    REQUIRE_FALSE(model.data(model.index(1, 0), SlotListModel::NameRole).isValid());
    REQUIRE_FALSE(model.data(QModelIndex(), SlotListModel::NameRole).isValid());
    // An unknown role number on a real row is not a row that is missing, but it
    // is equally not an answer.
    REQUIRE_FALSE(model.data(model.index(0, 0), Qt::UserRole - 1).isValid());
}
