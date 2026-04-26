// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "PairingDialog.h"

#include "Theme.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace dish::ui {

PairingDialog::PairingDialog(const models::DiscoveredServer& server, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Pair with %1").arg(server.name));
    setMinimumWidth(360);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto* header = new QLabel(QStringLiteral("PAIRING"), this);
    header->setStyleSheet(sectionHeaderQss());
    layout->addWidget(header);

    auto* msg =
        new QLabel(QStringLiteral("Enter the 6-digit PIN displayed on %1").arg(server.name), this);
    msg->setWordWrap(true);
    layout->addWidget(msg);

    pinEdit_ = new QLineEdit(this);
    pinEdit_->setMaxLength(6);
    pinEdit_->setPlaceholderText(QStringLiteral("PIN"));
    pinEdit_->setInputMethodHints(Qt::ImhDigitsOnly);
    layout->addWidget(pinEdit_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* ok = buttons->addButton(QStringLiteral("Pair"), QDialogButtonBox::AcceptRole);
    ok->setObjectName(QStringLiteral("primary"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString PairingDialog::pin() const { return pinEdit_->text().trimmed(); }

} // namespace dish::ui
