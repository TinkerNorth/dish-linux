// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The model is driven with hand-built ConnectionRow vectors, the same value the
// ConnectionCoordinator's composer emits. No live network is stood up.

#include "core/reducer/ConnectionRows.h"
#include "qml/ConnectionListModel.h"

#include <catch2/catch_test_macros.hpp>

#include <QObject>

#include <vector>

using dish::qml::ConnectionListModel;
namespace cm = dish::composer;
namespace rd = dish::reducer;

namespace {

// QSignalSpy stand-in: DishTests links Catch2, not Qt6::Test.
struct RowSpy {
    int inserts = 0;
    int removes = 0;
    int changes = 0;
    int firstRow = -1;
    int lastRow = -1;

    explicit RowSpy(QAbstractItemModel* model) {
        QObject::connect(model, &QAbstractItemModel::rowsInserted,
                         [this](const QModelIndex&, int first, int last) {
                             ++inserts;
                             firstRow = first;
                             lastRow = last;
                         });
        QObject::connect(model, &QAbstractItemModel::rowsRemoved,
                         [this](const QModelIndex&, int first, int last) {
                             ++removes;
                             firstRow = first;
                             lastRow = last;
                         });
        QObject::connect(
            model, &QAbstractItemModel::dataChanged,
            [this](const QModelIndex&, const QModelIndex&, const QList<int>&) { ++changes; });
    }
};

cm::ConnectionRow connectedRow(const std::string& id = "conn-1") {
    cm::ConnectionRow r;
    r.id = id;
    r.label = "Living Room";
    r.ip = "192.168.1.50";
    r.udpPort = 9876;
    r.live = rd::UiLinkState::Connected;
    r.kind = rd::ConnectionKind::Satellite;
    r.boundSlotId = "slot-1";
    r.glyph = rd::ConnectionGlyph::SatelliteConnected;
    r.dotColor = rd::DotColor::Success;
    r.chip = rd::StatusChipKey::Online;
    r.latencyOneWayMs = 3.4;
    r.latencySamples = 64;
    return r;
}

cm::ConnectionRow savedRow(const std::string& id = "conn-2") {
    cm::ConnectionRow r;
    r.id = id;
    r.label = "Office";
    r.ip = "192.168.1.51";
    r.udpPort = 9876;
    r.live = rd::UiLinkState::Saved;
    r.glyph = rd::ConnectionGlyph::SatelliteOff;
    r.dotColor = rd::DotColor::Muted;
    r.chip = rd::StatusChipKey::Offline;
    return r;
}

QVariant roleOf(const ConnectionListModel& model, int row, int role) {
    return model.data(model.index(row, 0), role);
}

} // namespace

TEST_CASE("ConnectionListModel: roleNames covers every Roles enumerator", "[connmodel][roles]") {
    ConnectionListModel model;
    const auto names = model.roleNames();
    REQUIRE(names.size() == 12);
    REQUIRE(names.value(ConnectionListModel::IdRole) == QByteArray("connectionId"));
    REQUIRE(names.value(ConnectionListModel::ChipRole) == QByteArray("chip"));
    REQUIRE(names.value(ConnectionListModel::LiveLinkRole) == QByteArray("liveLink"));
    REQUIRE(names.value(ConnectionListModel::LatencyTextRole) == QByteArray("latencyText"));
    REQUIRE(names.value(ConnectionListModel::LatencySamplesRole) == QByteArray("latencySamples"));
    QSet<QByteArray> unique;
    for (const auto& n : names) { unique.insert(n); }
    REQUIRE(unique.size() == names.size());
}

TEST_CASE("ConnectionListModel: rowCount tracks the pushed rows", "[connmodel][rows]") {
    ConnectionListModel model;
    REQUIRE(model.rowCount() == 0);
    model.setRows({connectedRow(), savedRow()});
    REQUIRE(model.rowCount() == 2);
    model.setRows({savedRow()});
    REQUIRE(model.rowCount() == 1);
}

TEST_CASE("ConnectionListModel: data maps a connected row's roles + tokens", "[connmodel][data]") {
    ConnectionListModel model;
    model.setRows({connectedRow()});

    REQUIRE(roleOf(model, 0, ConnectionListModel::IdRole).toString() == "conn-1");
    REQUIRE(roleOf(model, 0, ConnectionListModel::LabelRole).toString() == "Living Room");
    REQUIRE(roleOf(model, 0, ConnectionListModel::IpRole).toString() == "192.168.1.50");
    REQUIRE(roleOf(model, 0, ConnectionListModel::UdpPortRole).toInt() == 9876);
    REQUIRE(roleOf(model, 0, ConnectionListModel::LinkStateRole).toString() == "connected");
    REQUIRE(roleOf(model, 0, ConnectionListModel::ChipRole).toString() == "online");
    REQUIRE(roleOf(model, 0, ConnectionListModel::DotColorRole).toString() == "success");
    REQUIRE(roleOf(model, 0, ConnectionListModel::GlyphRole).toString() == "satelliteConnected");
    REQUIRE(roleOf(model, 0, ConnectionListModel::BoundSlotIdRole).toString() == "slot-1");
    REQUIRE(roleOf(model, 0, ConnectionListModel::LiveLinkRole).toBool());
    // Pre-formatted by the pure core helper so both UIs render the same figure.
    REQUIRE(roleOf(model, 0, ConnectionListModel::LatencyTextRole).toString() == "~3.4 ms");
    REQUIRE(roleOf(model, 0, ConnectionListModel::LatencySamplesRole).toInt() == 64);
}

TEST_CASE("ConnectionListModel: a saved row maps offline tokens + a non-live link",
          "[connmodel][data]") {
    ConnectionListModel model;
    model.setRows({savedRow()});
    REQUIRE(roleOf(model, 0, ConnectionListModel::LinkStateRole).toString() == "saved");
    REQUIRE(roleOf(model, 0, ConnectionListModel::ChipRole).toString() == "offline");
    REQUIRE(roleOf(model, 0, ConnectionListModel::DotColorRole).toString() == "muted");
    REQUIRE(roleOf(model, 0, ConnectionListModel::GlyphRole).toString() == "satelliteOff");
    REQUIRE(roleOf(model, 0, ConnectionListModel::BoundSlotIdRole).toString().isEmpty());
    REQUIRE_FALSE(roleOf(model, 0, ConnectionListModel::LiveLinkRole).toBool());
    // No samples, so no text; the QML caption also gates on linkState.
    REQUIRE(roleOf(model, 0, ConnectionListModel::LatencyTextRole).toString().isEmpty());
    REQUIRE(roleOf(model, 0, ConnectionListModel::LatencySamplesRole).toInt() == 0);
}

TEST_CASE("ConnectionListModel: appending emits rowsInserted only for the delta",
          "[connmodel][signals]") {
    ConnectionListModel model;
    model.setRows({connectedRow()});

    RowSpy spy(&model);
    model.setRows({connectedRow(), savedRow()});

    REQUIRE(spy.inserts == 1);
    REQUIRE(spy.removes == 0);
    REQUIRE(spy.firstRow == 1);
    REQUIRE(spy.lastRow == 1);
}

TEST_CASE("ConnectionListModel: removing emits rowsRemoved only for the delta",
          "[connmodel][signals]") {
    ConnectionListModel model;
    model.setRows({connectedRow(), savedRow()});

    RowSpy spy(&model);
    model.setRows({connectedRow()});

    REQUIRE(spy.removes == 1);
    REQUIRE(spy.inserts == 0);
    REQUIRE(spy.firstRow == 1);
    REQUIRE(spy.lastRow == 1);
}

TEST_CASE("ConnectionListModel: a same-count chip change emits dataChanged, not a reset",
          "[connmodel][signals]") {
    ConnectionListModel model;
    model.setRows({savedRow()});

    RowSpy spy(&model);

    auto r = savedRow();
    r.live = rd::UiLinkState::Connected; // same id/count, new state
    r.chip = rd::StatusChipKey::Online;
    model.setRows({r});

    REQUIRE(spy.inserts == 0);
    REQUIRE(spy.removes == 0);
    REQUIRE(spy.changes == 1);
    REQUIRE(roleOf(model, 0, ConnectionListModel::ChipRole).toString() == "online");
}

TEST_CASE("ConnectionListModel: countChanged fires on a row-count delta, not a same-count patch",
          "[connmodel][signals]") {
    // The bind-button enable rule binds to `count`, so an in-place patch must
    // not re-emit it or QML re-evaluates the gate needlessly.
    ConnectionListModel model;
    model.setRows({savedRow()});

    int countEmissions = 0;
    QObject::connect(&model, &ConnectionListModel::countChanged,
                     [&countEmissions] { ++countEmissions; });

    auto patched = savedRow();
    patched.live = rd::UiLinkState::Connected;
    model.setRows({patched});
    REQUIRE(countEmissions == 0);

    model.setRows({patched, connectedRow()});
    REQUIRE(countEmissions == 1);
    REQUIRE(model.count() == 2);

    model.setRows({patched});
    REQUIRE(countEmissions == 2);
    REQUIRE(model.count() == 1);
}
