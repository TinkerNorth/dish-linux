// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "PairingPage.h"

#include "DishLoaders.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace dish::ui {

PairingPage::PairingPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto* topBar = new QHBoxLayout;
    auto* backBtn = new QPushButton(tr("← Back"), this);
    backBtn->setFlat(true);
    title_ = new QLabel(tr("Pair"), this);
    title_->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
    topBar->addWidget(backBtn, 0, Qt::AlignVCenter);
    topBar->addWidget(title_, 1, Qt::AlignVCenter);
    layout->addLayout(topBar);

    auto* header = new QLabel(tr("PAIRING"), this);
    header->setStyleSheet(sectionHeaderQss());
    layout->addWidget(header);

    message_ = new QLabel(this);
    message_->setWordWrap(true);
    layout->addWidget(message_);

    pinEdit_ = new QLineEdit(this);
    pinEdit_->setMaxLength(4);
    pinEdit_->setPlaceholderText(QStringLiteral("0000"));
    pinEdit_->setInputMethodHints(Qt::ImhDigitsOnly);
    layout->addWidget(pinEdit_);

    layout->addStretch(1);

    auto* buttons = new QHBoxLayout;
    cancelBtn_ = new QPushButton(tr("Cancel"), this);
    // Pair button carries its loader *inside* itself — DishSpinner + label —
    // so the in-flight UI sits in the exact location the user just clicked,
    // matching dish-mac PairingSheet.
    pairBtn_ = new DishInFlightButton(tr("Pair"), this);
    pairBtn_->setObjectName(QStringLiteral("primary"));
    pairBtn_->setDefault(true);
    pairBtn_->setEnabled(false);

    buttons->addStretch(1);
    buttons->addWidget(cancelBtn_);
    buttons->addWidget(pairBtn_);
    layout->addLayout(buttons);

    QObject::connect(backBtn, &QPushButton::clicked, this, &PairingPage::cancelRequested);
    QObject::connect(cancelBtn_, &QPushButton::clicked, this, &PairingPage::cancelRequested);
    QObject::connect(pairBtn_, &QPushButton::clicked, this, &PairingPage::submit);
    QObject::connect(pinEdit_, &QLineEdit::returnPressed, this, &PairingPage::submit);
    QObject::connect(pinEdit_, &QLineEdit::textChanged, this,
                     [this](const QString&) { updatePairEnabled(); });
}

void PairingPage::updatePairEnabled() {
    pairBtn_->setEnabled(!pending_ && pinEdit_->text().trimmed().length() == 4);
}

void PairingPage::setPending(bool pending) {
    pending_ = pending;
    // In-button spinner mirrors dish-mac's `if isPairing` branch: swap the
    // label to "Pairing…" and show the DishSpinner next to it. The button is
    // disabled regardless (Pair-enabled is gated on `!pending_`) so the user
    // can't double-submit; the disabled-alpha rule in Theme.cpp carries the
    // not-tappable signal.
    pairBtn_->setInFlight(pending, tr("Pairing…"), tr("Pair"));
    pinEdit_->setEnabled(!pending);
    cancelBtn_->setEnabled(!pending);
    updatePairEnabled();
}

void PairingPage::setServer(const models::DiscoveredServer& server) {
    server_ = server;
    title_->setText(tr("Pair with %1").arg(server.name));
    message_->setText(tr("Enter the PIN shown on %1").arg(server.name));
    pinEdit_->clear();
    setPending(false);
    pinEdit_->setFocus();
}

void PairingPage::submit() {
    const auto pin = pinEdit_->text().trimmed();
    if (pin.length() != 4) { return; }
    emit pairRequested(server_, pin);
}

} // namespace dish::ui
