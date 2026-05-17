// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionsPage.h"

#include "AppModel.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace dish::ui {

ConnectionsPage::ConnectionsPage(AppModel* model, QWidget* parent)
    : QWidget(parent), model_(model) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);

    auto* topBar = new QHBoxLayout;
    auto* backBtn = new QPushButton(QStringLiteral("← Back"), this);
    backBtn->setFlat(true);
    auto* title = new QLabel(QStringLiteral("Connections"), this);
    title->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
    topBar->addWidget(backBtn, 0, Qt::AlignVCenter);
    topBar->addWidget(title, 1, Qt::AlignVCenter);
    layout->addLayout(topBar);

    scanSpinner_ = new QProgressBar(this);
    scanSpinner_->setRange(0, 0);
    scanSpinner_->setTextVisible(false);
    scanSpinner_->setFixedHeight(4);
    auto scanSp = scanSpinner_->sizePolicy();
    scanSp.setRetainSizeWhenHidden(true);
    scanSpinner_->setSizePolicy(scanSp);
    scanSpinner_->setVisible(false);
    layout->addWidget(scanSpinner_);

    auto* discoveredHeader = new QLabel(QStringLiteral("DISCOVERED"), this);
    discoveredHeader->setStyleSheet(sectionHeaderQss());
    layout->addWidget(discoveredHeader);

    discoveredList_ = new QListWidget(this);
    layout->addWidget(discoveredList_, 1);

    auto* row = new QHBoxLayout;
    scanButton_ = new QPushButton(QStringLiteral("Scan"), this);
    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(hex(Theme::muted)));
    connectButton_ = new QPushButton(QStringLiteral("Connect"), this);
    connectButton_->setObjectName(QStringLiteral("primary"));
    connectButton_->setEnabled(false);
    row->addWidget(scanButton_);
    row->addWidget(statusLabel_, 1);
    row->addWidget(connectButton_);
    layout->addLayout(row);

    auto* rememberedHeader = new QLabel(QStringLiteral("REMEMBERED"), this);
    rememberedHeader->setStyleSheet(sectionHeaderQss());
    layout->addWidget(rememberedHeader);

    rememberedList_ = new QListWidget(this);
    layout->addWidget(rememberedList_, 1);

    forgetButton_ = new QPushButton(QStringLiteral("Forget"), this);
    forgetButton_->setEnabled(false);
    auto* row2 = new QHBoxLayout;
    row2->addStretch(1);
    row2->addWidget(forgetButton_);
    layout->addLayout(row2);

    QObject::connect(backBtn, &QPushButton::clicked, this, &ConnectionsPage::backRequested);
    QObject::connect(scanButton_, &QPushButton::clicked, this, &ConnectionsPage::onScanClicked);
    QObject::connect(connectButton_, &QPushButton::clicked, this,
                     &ConnectionsPage::onConnectClicked);
    QObject::connect(forgetButton_, &QPushButton::clicked, this, &ConnectionsPage::onForgetClicked);
    QObject::connect(discoveredList_, &QListWidget::itemSelectionChanged, this, [this] {
        connectButton_->setEnabled(discoveredList_->currentItem() != nullptr);
    });
    QObject::connect(rememberedList_, &QListWidget::itemSelectionChanged, this, [this] {
        forgetButton_->setEnabled(rememberedList_->currentItem() != nullptr);
    });

    QObject::connect(model_->wifi(), &net::WifiConnectionManager::discoveredChanged, this,
                     &ConnectionsPage::rebuildLists);
    QObject::connect(model_->wifi(), &net::WifiConnectionManager::scanningChanged, this, [this] {
        const bool scanning = model_->wifi()->isScanning();
        scanButton_->setEnabled(!scanning);
        scanSpinner_->setVisible(scanning);
        statusLabel_->setText(scanning ? QStringLiteral("Scanning…") : QString());
    });
    QObject::connect(model_->hub(), &net::ConnectionHub::changed, this,
                     &ConnectionsPage::rebuildLists);

    rebuildLists();
}

void ConnectionsPage::rebuildLists() {
    discoveredList_->clear();
    for (const auto& s : model_->wifi()->discoveredServers()) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 • %2 • %3")
                .arg(s.name.isEmpty() ? s.ip : s.name, s.ip,
                     models::discoverySourceLabel(s.source)));
        item->setData(Qt::UserRole, QVariant::fromValue(s.id()));
        discoveredList_->addItem(item);
    }
    rememberedList_->clear();
    for (const auto& r : model_->wifi()->remembered()) {
        auto* conn = model_->wifi()->get(r.id);
        const QString liveTag = (conn != nullptr && conn->state() == net::WifiState::Connected)
                                    ? QStringLiteral(" • connected")
                                    : QString();
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 • %2%3").arg(r.name.isEmpty() ? r.ip : r.name, r.ip, liveTag));
        item->setData(Qt::UserRole, r.id);
        rememberedList_->addItem(item);
    }
    connectButton_->setEnabled(discoveredList_->currentItem() != nullptr);
    forgetButton_->setEnabled(rememberedList_->currentItem() != nullptr);
}

void ConnectionsPage::onScanClicked() { model_->wifi()->startDiscovery(); }

void ConnectionsPage::onConnectClicked() {
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

void ConnectionsPage::onForgetClicked() {
    auto* item = rememberedList_->currentItem();
    if (item == nullptr) { return; }
    model_->wifi()->forget(item->data(Qt::UserRole).toString());
}

} // namespace dish::ui
