// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A QAbstractListModel over the derived connection rows. Holds no logic: every
// role mirrors a composer::ConnectionRow field. Lives in dish_core, not the
// Quick target, so the unit tests link it without Qml.

#pragma once

#include "composer/ConnectionsComposer.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>

#include <vector>

namespace dish::qml {

class ConnectionListModel : public QAbstractListModel {
    Q_OBJECT
    // A bare QAbstractListModel exposes no `count` to QML — only the *view* does
    // — so `model.count` reads undefined and silently disables a `count > 0`
    // gate. Expose it explicitly.
    Q_PROPERTY(int count READ count NOTIFY countChanged)
  public:
    // LinkState/Chip/DotColor/Glyph vend the render-key TOKENS from
    // RenderTokens.h, never sentences; QML localizes and colours from them.
    enum Roles {
        IdRole = Qt::UserRole + 1,
        LabelRole,
        IpRole,
        UdpPortRole,
        LinkStateRole,
        ChipRole,
        DotColorRole,
        GlyphRole,
        BoundSlotIdRole,
        LiveLinkRole,
        LatencyTextRole,
        LatencySamplesRole,
    };
    Q_ENUM(Roles)

    explicit ConnectionListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const { return static_cast<int>(rows_.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Diffs the tail count and patches survivors with one dataChanged, so a
    // re-push on a quiet ~1 Hz latency tick never resets the ListView.
    void setRows(const std::vector<composer::ConnectionRow>& rows);

  signals:
    void countChanged();

  private:
    std::vector<composer::ConnectionRow> rows_;
};

} // namespace dish::qml
