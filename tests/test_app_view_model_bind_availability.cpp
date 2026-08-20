// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// AppViewModel::availableConnectionsForSlot cannot be exercised directly: the
// view model owns a full AppModel (SDL bridge, theme controller over qApp) and
// its network methods open real sockets the process would join on exit. So the
// two-stage derivation it performs is replicated below and pinned here instead.

#include "Models/Models.h"
#include "core/reducer/PickerVisibility.h"

#include <catch2/catch_test_macros.hpp>

#include <QList>
#include <QString>

#include <optional>

using dish::models::ConnectionSummary;
using dish::models::LinkState;
using dish::reducer::connectionsVisibleInPicker;

namespace {

ConnectionSummary conn(const QString& id, LinkState live,
                       std::optional<QString> boundSlotId = std::nullopt) {
    ConnectionSummary c;
    c.id = id;
    c.label = id;
    c.live = live;
    c.boundSlotId = std::move(boundSlotId);
    return c;
}

QList<QString> ids(const QList<ConnectionSummary>& list) {
    QList<QString> out;
    for (const auto& c : list) { out.push_back(c.id); }
    return out;
}

// Mirrors the invokable's derivation over MainUiState, before its
// {connectionId,label,dotColor,glyph} JS projection.
QList<ConnectionSummary> availableForSlot(const QList<ConnectionSummary>& connections,
                                          const QString& slotId,
                                          const std::optional<QString>& boundConnectionId) {
    QList<ConnectionSummary> candidates;
    for (const auto& c : connections) {
        const bool boundElsewhere = c.boundSlotId.has_value() && *c.boundSlotId != slotId;
        if (!boundElsewhere) { candidates.append(c); }
    }
    return connectionsVisibleInPicker(candidates, boundConnectionId);
}

} // namespace

TEST_CASE("bind availability: a connection bound to ANOTHER slot is excluded", "[appvm][bind]") {
    const QList<ConnectionSummary> connections{
        conn("s:free", LinkState::Connected),
        conn("s:other", LinkState::Connected, QStringLiteral("slot-B"))};

    const auto visible = availableForSlot(connections, QStringLiteral("slot-A"), std::nullopt);
    REQUIRE(ids(visible) == QList<QString>{"s:free"});
}

TEST_CASE("bind availability: an unbound live connection is included", "[appvm][bind]") {
    const QList<ConnectionSummary> connections{conn("s:free", LinkState::Connected)};
    const auto visible = availableForSlot(connections, QStringLiteral("slot-A"), std::nullopt);
    REQUIRE(ids(visible) == QList<QString>{"s:free"});
}

TEST_CASE("bind availability: an unbound OFFLINE connection is hidden", "[appvm][bind]") {
    // Saved is a remembered-but-down link, so it is not a usable pick.
    const QList<ConnectionSummary> connections{conn("s:offline", LinkState::Saved)};
    const auto visible = availableForSlot(connections, QStringLiteral("slot-A"), std::nullopt);
    REQUIRE(visible.isEmpty());
}

TEST_CASE("bind availability: the slot's OWN binding is held over even when offline",
          "[appvm][bind]") {
    const QList<ConnectionSummary> connections{
        conn("s:mine", LinkState::Saved, QStringLiteral("slot-A")),
        conn("s:free", LinkState::Connected)};

    const auto visible =
        availableForSlot(connections, QStringLiteral("slot-A"), std::optional<QString>("s:mine"));
    REQUIRE(ids(visible) == QList<QString>({"s:mine", "s:free"}));
}

TEST_CASE("bind availability: excluded-elsewhere + included-unbound + held-over together",
          "[appvm][bind]") {
    const QList<ConnectionSummary> connections{
        conn("s:mine", LinkState::Saved, QStringLiteral("slot-A")), // own holdover (offline)
        conn("s:elsewhere", LinkState::Connected, QStringLiteral("slot-B")), // bound elsewhere
        conn("s:free", LinkState::Connected),                                // unbound + available
        conn("s:offline", LinkState::Saved)};                                // unbound + offline

    const auto visible =
        availableForSlot(connections, QStringLiteral("slot-A"), std::optional<QString>("s:mine"));
    REQUIRE(ids(visible) == QList<QString>({"s:mine", "s:free"}));
}
