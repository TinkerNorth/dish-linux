// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "qml/SlotListModel.h"

#include "UI/SlotLiveStats.h"
#include "core/reducer/BatteryUi.h"
#include "core/reducer/LatencyWindow.h"
#include "core/reducer/SlotPathFields.h"
#include "qml/RenderTokens.h"

namespace dish::qml {

namespace {

constexpr std::uint8_t kBatteryLevelUnknown = 0xFF;

// Mirrors dish-android's phase strings, which are the shared vocabulary. Kept
// here rather than on the Qt-free FSM enum, which must not carry UI strings.
QString pathPhaseToken(reducer::UsbPhase phase) {
    switch (phase) {
    case reducer::UsbPhase::Routed:
        return QStringLiteral("routed");
    case reducer::UsbPhase::Claiming:
        return QStringLiteral("claiming");
    case reducer::UsbPhase::Direct:
        return QStringLiteral("direct");
    case reducer::UsbPhase::AwaitingFramework:
        return QStringLiteral("awaitingFramework");
    case reducer::UsbPhase::RestoreStuck:
        return QStringLiteral("restoreStuck");
    case reducer::UsbPhase::NeedsReplug:
        return QStringLiteral("needsReplug");
    }
    return QStringLiteral("routed"); // unreachable; total switch
}

// Only two strings appear: the stamped value is always the FSM's RESOLVED
// choice. "auto" is an input to setSlotPath, never a reflected desired.
QString desiredPathToken(reducer::PathChoice choice) {
    return choice == reducer::PathChoice::Direct ? QStringLiteral("direct")
                                                 : QStringLiteral("standard");
}

QString directFailureToken(const std::optional<reducer::DirectClaimFailure>& failure) {
    if (!failure.has_value()) { return {}; }
    switch (*failure) {
    case reducer::DirectClaimFailure::PermissionDenied:
        return QStringLiteral("permissionDenied");
    case reducer::DirectClaimFailure::Busy:
        return QStringLiteral("busy");
    case reducer::DirectClaimFailure::InitFailed:
        return QStringLiteral("initFailed");
    case reducer::DirectClaimFailure::Dropped:
        return QStringLiteral("dropped");
    }
    return {}; // unreachable; total switch
}

QString dotColorToken(const models::ControllerSlot& s) {
    if (!s.boundStatus.has_value()) { return QStringLiteral("muted"); }
    return s.boundStatus->live == models::LinkState::Connected ? QStringLiteral("success")
                                                               : QStringLiteral("warning");
}

} // namespace

SlotListModel::SlotListModel(QObject* parent) : QAbstractListModel(parent) {}

int SlotListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) { return 0; }
    return static_cast<int>(slots_.size());
}

QVariant SlotListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= slots_.size()) { return {}; }
    const auto& s = slots_.at(index.row());

    switch (role) {
    case IdRole:
        return s.id;
    case NameRole:
        return s.name;
    case BoundRole:
        return s.boundConnectionId.has_value();
    case BoundConnectionIdRole:
        return s.boundConnectionId.value_or(QString());
    case BoundLabelRole:
        return s.boundStatus.has_value() ? s.boundStatus->label : QString();
    case LiveRole:
        return s.boundStatus.has_value() && s.boundStatus->live == models::LinkState::Connected;
    case DotColorRole:
        return dotColorToken(s);
    case UsbDirectRole:
        return s.usbDirect;
    case BluetoothRole:
        return s.bluetooth;
    case RemappableRole:
        return s.remappable;
    case EmulateNameRole:
        return s.emulateName;
    case RegisteringRole:
        return s.registering;

    case HasMotionRole:
        return s.capabilities.hasMotion;
    case HasLightbarRole:
        return s.capabilities.hasLightbar;
    case HasTouchpadRole:
        return s.capabilities.hasTouchpad;
    case HasRumbleRole:
        return s.capabilities.hasRumble;
    case VerifiedModelRole:
        return s.verifiedModel;
    case MicArmedRole:
        return s.micArmed;
    case MicMutedRole:
        return s.micMuted;
    case BatteryLevelRole:
        return static_cast<int>(s.capabilities.batteryLevel);
    case BatteryStatusRole:
        return static_cast<int>(s.capabilities.batteryStatus);
    case BatteryKnownRole:
        return s.capabilities.batteryLevel != kBatteryLevelUnknown;
    case BatteryLowRole: {
        const auto ui = reducer::batteryUiFromWire(static_cast<int>(s.capabilities.batteryLevel),
                                                   static_cast<int>(s.capabilities.batteryStatus));
        return ui.has_value() && reducer::isLowBattery(*ui);
    }

    // Which rate chips render is the pure SlotLiveStats mapper's decision, not
    // the delegate's.
    case GamepadHzRole: {
        const auto chip = ui::gamepadRateChip(s.liveRates, s.usbDirect);
        return chip.hz;
    }
    case GamepadHzLiveRole: {
        const auto chip = ui::gamepadRateChip(s.liveRates, s.usbDirect);
        return chip.kind == ui::RateChipKind::Live;
    }
    case GamepadHzShownRole: {
        const auto chip = ui::gamepadRateChip(s.liveRates, s.usbDirect);
        return chip.kind != ui::RateChipKind::Hidden;
    }
    case MotionHzRole: {
        const auto chip =
            s.capabilities.hasMotion ? ui::motionRateChip(s.liveRates) : ui::RateChip{};
        return chip.hz;
    }
    case MotionHzShownRole: {
        const auto chip =
            s.capabilities.hasMotion ? ui::motionRateChip(s.liveRates) : ui::RateChip{};
        return chip.kind != ui::RateChipKind::Hidden;
    }
    case PollHzRole: {
        const auto chip = ui::pollRateChip(s.liveRates, s.usbDirect);
        return chip.hz;
    }
    case PollHzShownRole: {
        const auto chip = ui::pollRateChip(s.liveRates, s.usbDirect);
        return chip.kind != ui::RateChipKind::Hidden;
    }

    case SatIpRole: {
        const auto* row = rowForSlot(s);
        return row != nullptr ? QString::fromStdString(row->ip) : QString();
    }
    case SatLinkStateRole: {
        const auto* row = rowForSlot(s);
        return row != nullptr ? tokens::linkStateToken(row->live) : QString();
    }
    case SatChipRole: {
        const auto* row = rowForSlot(s);
        return row != nullptr ? tokens::chipToken(row->chip) : QString();
    }
    case SatDotColorRole: {
        const auto* row = rowForSlot(s);
        return row != nullptr ? tokens::dotToken(row->dotColor) : QString();
    }
    case SatGlyphRole: {
        const auto* row = rowForSlot(s);
        return row != nullptr ? tokens::glyphToken(row->glyph) : QString();
    }
    case SatLatencyTextRole: {
        // Same formatter and samples gate as ConnectionListModel, so the Home
        // wire label and the Connections row can never disagree.
        const auto* row = rowForSlot(s);
        return row != nullptr && row->latencySamples > 0
                   ? QString::fromStdString(reducer::formatLatencyMs(row->latencyOneWayMs))
                   : QString();
    }
    case SatLatencySamplesRole: {
        const auto* row = rowForSlot(s);
        return row != nullptr ? row->latencySamples : 0;
    }

    case PathPhaseRole:
        return pathPhaseToken(s.pathPhase);
    case DesiredPathRole:
        return desiredPathToken(s.desiredPath);
    case PathSupportedRole:
        return s.pathSupported;
    case ClaimInProgressRole:
        // Wider than the FSM phase: a switch is not done until the device form
        // and its telemetry settle too (Direct until the synthetic's poll rate
        // is measured, Standard until the synthetic is gone). That is what lets
        // the spinner end on a real event rather than an arbitrary timer.
        return reducer::slotPathSwitching(s.pathPhase, s.desiredPath, s.usbDirect,
                                          s.liveRates.directPollHz, s.directFailure.has_value());
    case DirectFailureRole:
        return directFailureToken(s.directFailure);
    default:
        return {};
    }
}

QHash<int, QByteArray> SlotListModel::roleNames() const {
    return {
        {IdRole, "slotId"},
        {NameRole, "name"},
        {BoundRole, "bound"},
        {BoundConnectionIdRole, "boundConnectionId"},
        {BoundLabelRole, "boundLabel"},
        {LiveRole, "live"},
        {DotColorRole, "dotColor"},
        {UsbDirectRole, "usbDirect"},
        {BluetoothRole, "bluetooth"},
        {RemappableRole, "remappable"},
        {EmulateNameRole, "emulateName"},
        {RegisteringRole, "registering"},
        {HasMotionRole, "hasMotion"},
        {HasLightbarRole, "hasLightbar"},
        {BatteryLevelRole, "batteryLevel"},
        {BatteryStatusRole, "batteryStatus"},
        {BatteryKnownRole, "batteryKnown"},
        {BatteryLowRole, "batteryLow"},
        {GamepadHzRole, "gamepadHz"},
        {GamepadHzLiveRole, "gamepadHzLive"},
        {GamepadHzShownRole, "gamepadHzShown"},
        {MotionHzRole, "motionHz"},
        {MotionHzShownRole, "motionHzShown"},
        {PollHzRole, "pollHz"},
        {PollHzShownRole, "pollHzShown"},
        {PathPhaseRole, "pathPhase"},
        {DesiredPathRole, "desiredPath"},
        {PathSupportedRole, "pathSupported"},
        {ClaimInProgressRole, "claimInProgress"},
        {DirectFailureRole, "directFailure"},
        {SatIpRole, "satIp"},
        {SatLinkStateRole, "satLinkState"},
        {SatChipRole, "satChip"},
        {SatDotColorRole, "satDotColor"},
        {SatGlyphRole, "satGlyph"},
        {SatLatencyTextRole, "satLatencyText"},
        {SatLatencySamplesRole, "satLatencySamples"},
        {HasTouchpadRole, "hasTouchpad"},
        {HasRumbleRole, "hasRumble"},
        {VerifiedModelRole, "verifiedModel"},
        {MicArmedRole, "micArmed"},
        {MicMutedRole, "micMuted"},
    };
}

void SlotListModel::setState(const QList<models::ControllerSlot>& slotList) {
    const int oldCount = static_cast<int>(slots_.size());
    const int newCount = static_cast<int>(slotList.size());

    // Row identity is POSITIONAL: this leans on AppModel emitting a
    // stable-ordered list (virtual slot first, then attached pads), so only the
    // tail delta is inserted/removed and survivors are patched in place.
    if (newCount > oldCount) {
        beginInsertRows({}, oldCount, newCount - 1);
        slots_ = slotList;
        endInsertRows();
    } else if (newCount < oldCount) {
        beginRemoveRows({}, newCount, oldCount - 1);
        slots_ = slotList;
        endRemoveRows();
    } else {
        slots_ = slotList;
    }

    const int common = newCount < oldCount ? newCount : oldCount;
    if (common > 0) { emit dataChanged(index(0), index(common - 1)); }

    if (oldCount != newCount) { emit countChanged(); }
}

void SlotListModel::setConnectionRows(const std::vector<composer::ConnectionRow>& rows) {
    connectionRows_ = rows;
    // Scoped to the join roles so the ~1 Hz latency tick does not re-evaluate
    // every slot-side binding.
    if (!slots_.isEmpty()) {
        static const QList<int> kJoinRoles{
            SatIpRole,    SatLinkStateRole,   SatChipRole,          SatDotColorRole,
            SatGlyphRole, SatLatencyTextRole, SatLatencySamplesRole};
        emit dataChanged(index(0), index(static_cast<int>(slots_.size()) - 1), kJoinRoles);
    }
}

const composer::ConnectionRow* SlotListModel::rowForSlot(const models::ControllerSlot& slot) const {
    if (!slot.boundConnectionId.has_value()) { return nullptr; }
    const std::string id = slot.boundConnectionId->toStdString();
    for (const auto& row : connectionRows_) {
        if (row.id == id) { return &row; }
    }
    return nullptr;
}

} // namespace dish::qml
