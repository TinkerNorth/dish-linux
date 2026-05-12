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
