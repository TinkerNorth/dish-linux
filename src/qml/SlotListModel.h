// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A QAbstractListModel over the AppModel's slot list. Holds no logic: every
// role mirrors a models::ControllerSlot field or a pure mapper's output. Lives
// in dish_core, not the Quick target, so the unit tests link it without Qml.

#pragma once

#include "Models/Models.h"
#include "composer/ConnectionsComposer.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <vector>

namespace dish::qml {

class SlotListModel : public QAbstractListModel {
    Q_OBJECT
    // A bare QAbstractListModel exposes no `count` to QML — only the *view* does
    // — so `model.count` reads undefined and silently disables a `count > 0`
    // gate. Expose it explicitly.
    Q_PROPERTY(int count READ count NOTIFY countChanged)
  public:
    // The QString roles vend render-key TOKENS, never sentences; QML localizes
    // and colours from them. The Sat* roles are the bound connection row joined
    // in by setConnectionRows, sharing ConnectionListModel's vocabulary so the
    // two surfaces cannot disagree; they read empty when the binding has no row.
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        BoundRole,
        BoundConnectionIdRole,
        BoundLabelRole,
        LiveRole,
        DotColorRole,
        UsbDirectRole,
        BluetoothRole,
        RemappableRole,
        EmulateNameRole,
        RegisteringRole,

        HasMotionRole,
        HasLightbarRole,
        BatteryLevelRole,  // 0..100, or 255 (0xFF) when unknown
        BatteryStatusRole, // kBatteryStatus* wire constant
        BatteryKnownRole,
        // The one low-battery decision, from reducer::isLowBattery. A delegate
        // that re-derives it from level and status will drift: the rule folds
        // the charging, full AND wired statuses, because a wired pad has
        // nothing to drain.
        BatteryLowRole,

        GamepadHzRole,
        GamepadHzLiveRole,
        GamepadHzShownRole,
        MotionHzRole,
        MotionHzShownRole,
        PollHzRole,
        PollHzShownRole,

        PathPhaseRole,
        DesiredPathRole,
        PathSupportedRole,
        ClaimInProgressRole,
        DirectFailureRole,

        SatIpRole,
        SatLinkStateRole,
        SatChipRole,
        SatDotColorRole,
        SatGlyphRole,
        SatLatencyTextRole,
        SatLatencySamplesRole,

        HasTouchpadRole,
        HasRumbleRole,
        VerifiedModelRole,

        // Controller audio: whether the slot's descriptor claims a mic (the
        // mute control's visibility) and the LOCAL mute truth.
        MicArmedRole,
        MicMutedRole,
    };
    Q_ENUM(Roles)

    explicit SlotListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const { return static_cast<int>(slots_.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Diffs the tail count and patches survivors with one dataChanged, so a
    // quiet 1 Hz telemetry tick never resets the ListView.
    void setState(const QList<models::ControllerSlot>& slotList);

    // Pushed on the coordinator's connectionsChanged. Scopes its dataChanged to
    // the join roles so the ~1 Hz latency tick refreshes the Home wire labels
    // without re-evaluating every slot-side binding.
    void setConnectionRows(const std::vector<composer::ConnectionRow>& rows);

  signals:
    void countChanged();

  private:
    // nullptr when unbound or the bound id has no row. Linear scan; both lists
    // are a handful of entries.
    const composer::ConnectionRow* rowForSlot(const models::ControllerSlot& slot) const;

    QList<models::ControllerSlot> slots_;
    std::vector<composer::ConnectionRow> connectionRows_;
};

} // namespace dish::qml
