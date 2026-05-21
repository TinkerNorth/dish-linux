// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SlotCard.h"

#include "BrandIcon.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

namespace dish::ui {

namespace {

// Battery wire status constants — mirrors SatelliteClient::kBatteryStatus*
// and satellite/src/core/types.h. Duplicated here as plain locals so the UI
// translation unit doesn't have to pull in the network header.
constexpr std::uint8_t kBatteryLevelUnknown = 0xFF;
constexpr std::uint8_t kBatteryStatusCharging = 2;
constexpr std::uint8_t kBatteryStatusFull = 3;
constexpr std::uint8_t kBatteryStatusWired = 4;

// Below this percentage the battery chip switches to the amber warning style.
constexpr std::uint8_t kLowBatteryThreshold = 15;

} // namespace

SlotCard::SlotCard(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("card"));
    setFrameShape(QFrame::NoFrame);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(12);

    // v6 brand satellite glyph anchored at the row leading edge. Each
    // slot routes to a satellite server, so the silhouette is the
    // satellite — the same icon family the ConnectionsPage rows render.
    // The 8px coloured dot is overlaid in the bottom-right corner of the
    // glyph as a secondary tonal cue (live=green, transient=amber,
    // otherwise=muted).
    glyph_ = new QLabel(this);
    setBrandIcon(glyph_, BrandIconKind::Satellite, models::LinkState::Saved, 28);

    dot_ = new QLabel(this);
    dot_->setFixedSize(8, 8);
    dot_->setStyleSheet(dotQss(Theme::muted));

    auto* textLayout = new QVBoxLayout;
    textLayout->setSpacing(2);
    nameLabel_ = new QLabel(this);
    nameLabel_->setStyleSheet(
        QStringLiteral("font-weight: 600; color: %1;").arg(hex(Theme::onSurface)));
    boundLabel_ = new QLabel(this);
    boundLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(hex(Theme::muted)));
    textLayout->addWidget(nameLabel_);
    textLayout->addWidget(boundLabel_);

    // Capability-chip row. Each chip says the controller HAS a piece of
    // hardware; its colour says whether the feature is active. The motion
    // chip is always shown — coloured "Gyro" when the pad has an IMU,
    // dimmed "No gyro" when it doesn't — so a player can tell "off" apart
    // from "not available". The battery chip joins it once a battery sample
    // arrives. Mirrors the dish-mac SlotCard capability row.
    capabilityRow_ = new QHBoxLayout;
    capabilityRow_->setSpacing(6);
    capabilityRow_->setContentsMargins(0, 4, 0, 0);
    motionChip_ = new QLabel(this);
    capabilityRow_->addWidget(motionChip_, 0, Qt::AlignVCenter);
    lightbarChip_ = new QLabel(this);
    capabilityRow_->addWidget(lightbarChip_, 0, Qt::AlignVCenter);
    batteryChip_ = new QLabel(this);
    capabilityRow_->addWidget(batteryChip_, 0, Qt::AlignVCenter);
    capabilityRow_->addStretch(1);
    textLayout->addLayout(capabilityRow_);

    bindButton_ = new QPushButton(this);
    QObject::connect(bindButton_, &QPushButton::clicked, this, &SlotCard::onBindClicked);

    layout->addWidget(glyph_, 0, Qt::AlignVCenter);
    layout->addWidget(dot_, 0, Qt::AlignVCenter);
    layout->addLayout(textLayout, 1);
    layout->addWidget(bindButton_, 0, Qt::AlignVCenter);
}

void SlotCard::setSlot(const models::ControllerSlot& slot,
                       const QList<models::ConnectionSummary>& available) {
    slot_ = slot;
    available_ = available;
    nameLabel_->setText(slot.name);
    if (slot.boundStatus.has_value()) {
        boundLabel_->setText(tr("Bound to %1").arg(slot.boundStatus->label));
        const auto color = slot.boundStatus->live == models::LinkState::Connected ? Theme::success
                                                                                  : Theme::warning;
        dot_->setStyleSheet(dotQss(color));
        setBrandIcon(glyph_, BrandIconKind::Satellite, slot.boundStatus->live, 28);
        bindButton_->setText(tr("Unbind"));
    } else {
        boundLabel_->setText(tr("Unbound"));
        dot_->setStyleSheet(dotQss(Theme::muted));
        setBrandIcon(glyph_, BrandIconKind::Satellite, models::LinkState::Saved, 28);
        bindButton_->setText(tr("Bind\u2026"));
    }
    bindButton_->setEnabled(slot.boundConnectionId.has_value() || !available.isEmpty());
    updateCapabilities();
}

void SlotCard::updateCapabilities() {
    // Two unambiguous states for motion. dish-linux has no per-feature on/off
    // setting, so a controller with an IMU always forwards motion — there is
    // no "available but off" state to render here; the chip is "Gyro" (on)
    // when the hardware is present and "No gyro" (dimmed) when it is not.
    const bool hasMotion = slot_.capabilities.hasMotion;
    motionChip_->setText(hasMotion ? tr("Gyro") : tr("No gyro"));
    motionChip_->setStyleSheet(capabilityChipQss(hasMotion));
    motionChip_->setToolTip(hasMotion ? tr("Gyro and accelerometer detected — motion is forwarded "
                                           "to the satellite for gyro aim.")
                                      : tr("This controller has no motion sensor — gyro aim is "
                                           "unavailable. Xbox pads have no gyro; DualSense, "
                                           "DualShock 4 and Switch Pro pads do."));

    // Lightbar-capability chip. Unlike the motion chip, this is shown ONLY
    // when the pad actually has an addressable RGB LED (DualSense / DS4) —
    // most pads have no lightbar and a "no lightbar" callout would be noise.
    // Styled with the filled "present" pill, matching the motion/battery
    // chips. The host game drives the colour over MSG_LIGHTBAR; the "Light
    // bar" setting can switch that off without changing this hardware chip.
    const bool hasLightbar = slot_.capabilities.hasLightbar;
    lightbarChip_->setVisible(hasLightbar);
    if (hasLightbar) {
        lightbarChip_->setText(tr("Lightbar"));
        lightbarChip_->setStyleSheet(capabilityChipQss(true));
        lightbarChip_->setToolTip(tr("Lightbar available — this controller has an RGB LED. "
                                     "It follows the host game's colour unless the Light bar "
                                     "setting is Off."));
    }

    // Battery chip. The (level, status) pair comes off the same MSG_BATTERY
    // sample the satellite receives: a wireless pad's own charge, or — for a
    // wired/unknown pad — the host machine's battery (a laptop's percentage,
    // or 100 % / WIRED on a desktop). Hidden until the first 30 s poll lands a
    // real reading; an unknown level (0xFF) has nothing meaningful to show.
    const std::uint8_t batteryLevel = slot_.capabilities.batteryLevel;
    const std::uint8_t batteryStatus = slot_.capabilities.batteryStatus;
    if (batteryLevel == kBatteryLevelUnknown) {
        batteryChip_->setVisible(false);
    } else {
        batteryChip_->setVisible(true);
        const bool charging = batteryStatus == kBatteryStatusCharging;
        const bool wired = batteryStatus == kBatteryStatusWired;
        const bool full = batteryStatus == kBatteryStatusFull;
        // A wired/full pad at 100 % is never "low"; only an actually-draining
        // pack trips the warning style.
        const bool lowBattery = batteryLevel < kLowBatteryThreshold && !charging && !wired;
        QString label = tr("Battery %1%").arg(batteryLevel);
        if (charging) {
            label = tr("Battery %1% ↑").arg(batteryLevel); // up arrow
        } else if (wired) {
            label = tr("Battery wired");
        } else if (full) {
            label = tr("Battery full");
        }
        batteryChip_->setText(label);
        batteryChip_->setStyleSheet(batteryChipQss(lowBattery));
        QString tip;
        if (wired) {
            tip = tr("This host has no internal battery (a desktop) — "
                     "reported as wired / full charge.");
        } else if (charging) {
            tip = tr("Battery at %1% and charging.").arg(batteryLevel);
        } else if (full) {
            tip = tr("Battery full (%1%).").arg(batteryLevel);
        } else if (lowBattery) {
            tip = tr("Battery low — %1% remaining.").arg(batteryLevel);
        } else {
            tip = tr("Battery at %1%.").arg(batteryLevel);
        }
        batteryChip_->setToolTip(tip);
    }
}

void SlotCard::onBindClicked() {
    if (slot_.boundConnectionId.has_value()) {
        emit unbindRequested(slot_.id);
        return;
    }
    if (available_.isEmpty()) { return; }
    QMenu menu(this);
    // Each entry in the bind picker IS a satellite server (dish-linux is
    // Wi-Fi-only, so no Bluetooth kind to branch on). Use the same v6
    // brand satellite glyph the ConnectionsPage rows and the SlotCard
    // leading-edge glyph use, keyed on the same LinkState — mirrors the
    // dish-mac SlotCard expanded picker and the dish-android
    // ControllerAdapter.buildConnectionHeader() bind list.
    for (const auto& c : available_) {
        auto* act = menu.addAction(brandIcon(BrandIconKind::Satellite, c.live, 16, this), c.label);
        const QString cid = c.id;
        QObject::connect(act, &QAction::triggered, this,
                         [this, cid] { emit bindRequested(slot_.id, cid); });
    }
    menu.exec(bindButton_->mapToGlobal(QPoint(0, bindButton_->height())));
}

} // namespace dish::ui
