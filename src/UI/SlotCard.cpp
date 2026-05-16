// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SlotCard.h"

#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

namespace dish::ui {

SlotCard::SlotCard(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("card"));
    setFrameShape(QFrame::NoFrame);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(12);

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
    // from "not available". Mirrors the dish-mac SlotCard capability row.
    capabilityRow_ = new QHBoxLayout;
    capabilityRow_->setSpacing(6);
    capabilityRow_->setContentsMargins(0, 4, 0, 0);
    motionChip_ = new QLabel(this);
    capabilityRow_->addWidget(motionChip_, 0, Qt::AlignVCenter);
    capabilityRow_->addStretch(1);
    textLayout->addLayout(capabilityRow_);

    bindButton_ = new QPushButton(this);
    QObject::connect(bindButton_, &QPushButton::clicked, this, &SlotCard::onBindClicked);

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
        boundLabel_->setText(QStringLiteral("Bound to %1").arg(slot.boundStatus->label));
        const auto color = slot.boundStatus->live == models::ConnectionLive::Connected
                               ? Theme::success
                               : Theme::warning;
        dot_->setStyleSheet(dotQss(color));
        bindButton_->setText(QStringLiteral("Unbind"));
    } else {
        boundLabel_->setText(QStringLiteral("Unbound"));
        dot_->setStyleSheet(dotQss(Theme::muted));
        bindButton_->setText(QStringLiteral("Bind\u2026"));
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
    motionChip_->setText(hasMotion ? QStringLiteral("Gyro")
                                   : QStringLiteral("No gyro"));
    motionChip_->setStyleSheet(capabilityChipQss(hasMotion));
    motionChip_->setToolTip(
        hasMotion
            ? QStringLiteral("Gyro and accelerometer detected — motion is forwarded "
                             "to the satellite for gyro aim.")
            : QStringLiteral("This controller has no motion sensor — gyro aim is "
                             "unavailable. Xbox pads have no gyro; DualSense, "
                             "DualShock 4 and Switch Pro pads do."));
}

void SlotCard::onBindClicked() {
    if (slot_.boundConnectionId.has_value()) {
        emit unbindRequested(slot_.id);
        return;
    }
    if (available_.isEmpty()) { return; }
    QMenu menu(this);
    for (const auto& c : available_) {
        auto* act = menu.addAction(c.label);
        const QString cid = c.id;
        QObject::connect(act, &QAction::triggered, this,
                         [this, cid] { emit bindRequested(slot_.id, cid); });
    }
    menu.exec(bindButton_->mapToGlobal(QPoint(0, bindButton_->height())));
}

} // namespace dish::ui
