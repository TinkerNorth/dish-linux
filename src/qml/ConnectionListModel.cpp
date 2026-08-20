// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "qml/ConnectionListModel.h"

#include "core/reducer/ConnectionRows.h"
#include "core/reducer/LatencyWindow.h"
#include "qml/RenderTokens.h"

namespace dish::qml {

namespace {

namespace rd = dish::reducer;
using tokens::chipToken;
using tokens::dotToken;
using tokens::glyphToken;
using tokens::linkStateToken;

} // namespace

ConnectionListModel::ConnectionListModel(QObject* parent) : QAbstractListModel(parent) {}

int ConnectionListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) { return 0; }
    return static_cast<int>(rows_.size());
}

QVariant ConnectionListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& r = rows_.at(static_cast<std::size_t>(index.row()));

    switch (role) {
    case IdRole:
        return QString::fromStdString(r.id);
    case LabelRole:
        return QString::fromStdString(r.label);
    case IpRole:
        return QString::fromStdString(r.ip);
    case UdpPortRole:
        return r.udpPort;
    case LinkStateRole:
        return linkStateToken(r.live);
    case ChipRole:
        return chipToken(r.chip);
    case DotColorRole:
        return dotToken(r.dotColor);
    case GlyphRole:
        return glyphToken(r.glyph);
    case BoundSlotIdRole:
        return QString::fromStdString(r.boundSlotId);
    case LiveLinkRole:
        return rd::isLiveLink(r.live);
    case LatencyTextRole:
        // Empty until the window has samples — never a fabricated "~0.0 ms".
        return r.latencySamples > 0 ? QString::fromStdString(rd::formatLatencyMs(r.latencyOneWayMs))
                                    : QString();
    case LatencySamplesRole:
        return r.latencySamples;
    default:
        return {};
    }
}

QHash<int, QByteArray> ConnectionListModel::roleNames() const {
    return {
        {IdRole, "connectionId"},
        {LabelRole, "label"},
        {IpRole, "ip"},
        {UdpPortRole, "udpPort"},
        {LinkStateRole, "linkState"},
        {ChipRole, "chip"},
        {DotColorRole, "dotColor"},
        {GlyphRole, "glyph"},
        {BoundSlotIdRole, "boundSlotId"},
        {LiveLinkRole, "liveLink"},
        {LatencyTextRole, "latencyText"},
        {LatencySamplesRole, "latencySamples"},
    };
}

void ConnectionListModel::setRows(const std::vector<composer::ConnectionRow>& rows) {
    const int oldCount = static_cast<int>(rows_.size());
    const int newCount = static_cast<int>(rows.size());

    if (newCount > oldCount) {
        beginInsertRows({}, oldCount, newCount - 1);
        rows_ = rows;
        endInsertRows();
    } else if (newCount < oldCount) {
        beginRemoveRows({}, newCount, oldCount - 1);
        rows_ = rows;
        endRemoveRows();
    } else {
        rows_ = rows;
    }

    const int common = newCount < oldCount ? newCount : oldCount;
    if (common > 0) { emit dataChanged(index(0), index(common - 1)); }

    if (oldCount != newCount) { emit countChanged(); }
}

} // namespace dish::qml
