// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionsDialog.h"

#include "AppModel.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace dish::ui {

ConnectionsDialog::ConnectionsDialog(AppModel* model, QWidget* parent)
    : QDialog(parent), model_(model) {
    setWindowTitle(QStringLiteral("Connections"));
    setMinimumSize(560, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);

    auto* discoveredHeader = new QLabel(QStringLiteral("DISCOVERED"), this);
    discoveredHeader->setStyleSheet(sectionHeaderQss());
    layout->addWidget(discoveredHeader);

    discoveredList_ = new QListWidget(this);
    layout->addWidget(discoveredList_, 1);

    auto* row = new QHBoxLayout;
    scanButton_ = new QPushButton(QStringLiteral("Scan"), this);
    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(hex(Theme::muted)));
    auto* connectBtn = new QPushButton(QStringLiteral("Connect"), this);
    connectBtn->setObjectName(QStringLiteral("primary"));
    row->addWidget(scanButton_);
    row->addWidget(statusLabel_, 1);
    row->addWidget(connectBtn);
    layout->addLayout(row);

    auto* rememberedHeader = new QLabel(QStringLiteral("REMEMBERED"), this);
    rememberedHeader->setStyleSheet(sectionHeaderQss());
    layout->addWidget(rememberedHeader);

    rememberedList_ = new QListWidget(this);
    layout->addWidget(rememberedList_, 1);

    auto* forgetBtn = new QPushButton(QStringLiteral("Forget"), this);
    auto* row2 = new QHBoxLayout;
    row2->addStretch(1);
    row2->addWidget(forgetBtn);
    layout->addLayout(row2);

    QObject::connect(scanButton_, &QPushButton::clicked, this, &ConnectionsDialog::onScanClicked);
    QObject::connect(connectBtn, &QPushButton::clicked, this, &ConnectionsDialog::onConnectClicked);
    QObject::connect(forgetBtn, &QPushButton::clicked, this, &ConnectionsDialog::onForgetClicked);

    QObject::connect(model_->wifi(), &net::WifiConnectionManager::discoveredChanged, this,
                     &ConnectionsDialog::rebuildLists);
    QObject::connect(model_->wifi(), &net::WifiConnectionManager::scanningChanged, this, [this] {
        scanButton_->setEnabled(!model_->wifi()->isScanning());
        statusLabel_->setText(model_->wifi()->isScanning() ? QStringLiteral("Scanning\u2026")
                                                           : QString());
    });
    QObject::connect(model_->hub(), &net::ConnectionHub::changed, this,
                     &ConnectionsDialog::rebuildLists);

    rebuildLists();
}

void ConnectionsDialog::rebuildLists() {
    discoveredList_->clear();
    for (const auto& s : model_->wifi()->discoveredServers()) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 \u2022 %2").arg(s.name.isEmpty() ? s.ip : s.name, s.ip));
        item->setData(Qt::UserRole, QVariant::fromValue(s.id()));
        discoveredList_->addItem(item);
    }
    rememberedList_->clear();
    for (const auto& r : model_->wifi()->remembered()) {
        auto* conn = model_->wifi()->get(r.id);
        const QString liveTag = (conn != nullptr && conn->state() == net::WifiState::Connected)
                                    ? QStringLiteral(" \u2022 connected")
                                    : QString();
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 \u2022 %2%3").arg(r.name.isEmpty() ? r.ip : r.name, r.ip, liveTag));
        item->setData(Qt::UserRole, r.id);
        rememberedList_->addItem(item);
    }
}

void ConnectionsDialog::onScanClicked() { model_->wifi()->startDiscovery(); }

void ConnectionsDialog::onConnectClicked() {
    auto* item = discoveredList_->currentItem();
    if (item == nullptr) { return; }
    const auto wantedId = item->data(Qt::UserRole).toString();
    for (const auto& s : model_->wifi()->discoveredServers()) {
        if (s.id() == wantedId) {
            model_->wifi()->connectTo(s);
            return;
        }
    }
}

void ConnectionsDialog::onForgetClicked() {
    auto* item = rememberedList_->currentItem();
    if (item == nullptr) { return; }
    model_->wifi()->forget(item->data(Qt::UserRole).toString());
}

} // namespace dish::ui
