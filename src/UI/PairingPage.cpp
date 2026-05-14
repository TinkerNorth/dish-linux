// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "PairingPage.h"

#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace dish::ui {

PairingPage::PairingPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto* topBar = new QHBoxLayout;
    auto* backBtn = new QPushButton(QStringLiteral("← Back"), this);
    backBtn->setFlat(true);
    title_ = new QLabel(QStringLiteral("Pair"), this);
    title_->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
    topBar->addWidget(backBtn, 0, Qt::AlignVCenter);
    topBar->addWidget(title_, 1, Qt::AlignVCenter);
    layout->addLayout(topBar);

    spinner_ = new QProgressBar(this);
    spinner_->setRange(0, 0); // indeterminate
    spinner_->setTextVisible(false);
    spinner_->setFixedHeight(4);
    auto spinnerSp = spinner_->sizePolicy();
    spinnerSp.setRetainSizeWhenHidden(true);
    spinner_->setSizePolicy(spinnerSp);
    spinner_->setVisible(false);
    layout->addWidget(spinner_);

    auto* header = new QLabel(QStringLiteral("PAIRING"), this);
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
    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    pairBtn_ = new QPushButton(QStringLiteral("Pair"), this);
    pairBtn_->setObjectName(QStringLiteral("primary"));
    pairBtn_->setDefault(true);
    pairBtn_->setEnabled(false);
    buttons->addStretch(1);
    buttons->addWidget(cancelBtn);
    buttons->addWidget(pairBtn_);
    layout->addLayout(buttons);

    QObject::connect(backBtn, &QPushButton::clicked, this, &PairingPage::cancelRequested);
    QObject::connect(cancelBtn, &QPushButton::clicked, this, &PairingPage::cancelRequested);
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
    spinner_->setVisible(pending);
    pinEdit_->setEnabled(!pending);
    updatePairEnabled();
}

void PairingPage::setServer(const models::DiscoveredServer& server) {
    server_ = server;
    title_->setText(QStringLiteral("Pair with %1").arg(server.name));
    message_->setText(QStringLiteral("Enter the PIN shown on %1").arg(server.name));
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
