// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionsPage.h"

#include "AppModel.h"
#include "BrandIcon.h"
#include "DishLoaders.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
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

    // Section header: brand satellite glyph + "FOUND" label.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* glyph = new QLabel(this);
        setBrandIcon(glyph, BrandIconKind::Satellite, models::LinkState::Saved, 18);
        auto* discoveredHeader = new QLabel(QStringLiteral("FOUND"), this);
        discoveredHeader->setStyleSheet(sectionHeaderQss());
        row->addWidget(glyph, 0, Qt::AlignVCenter);
        row->addWidget(discoveredHeader, 1, Qt::AlignVCenter);
        layout->addLayout(row);
    }

    discoveredList_ = new QListWidget(this);
    layout->addWidget(discoveredList_, 1);

    auto* row = new QHBoxLayout;
    // Scan + Connect carry their loader *inside* the button itself: the
    // Scan button shows DishSpinner + "Scanning…" while a network
    // discovery is in flight, and the Connect button shows DishSpinner +
    // "Connecting…" while the chosen server is in LinkState::Connecting.
    // DishInFlightButton handles the spinner/label layout + size hint.
    scanButton_ = new DishInFlightButton(QStringLiteral("Scan"), this);
    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(hex(Theme::muted)));
    connectButton_ = new DishInFlightButton(QStringLiteral("Connect"), this);
    connectButton_->setObjectName(QStringLiteral("primary"));
    connectButton_->setEnabled(false);
    row->addWidget(scanButton_);
    row->addWidget(statusLabel_, 1);
    row->addWidget(connectButton_);
    layout->addLayout(row);

    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* glyph = new QLabel(this);
        // REMEMBERED is a list of satellite servers — same brand family as
        // FOUND just above so the two halves of the page read consistently.
        setBrandIcon(glyph, BrandIconKind::Satellite, models::LinkState::Saved, 18);
        auto* rememberedHeader = new QLabel(QStringLiteral("REMEMBERED"), this);
        rememberedHeader->setStyleSheet(sectionHeaderQss());
        row->addWidget(glyph, 0, Qt::AlignVCenter);
        row->addWidget(rememberedHeader, 1, Qt::AlignVCenter);
        layout->addLayout(row);
    }

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
    QObject::connect(discoveredList_, &QListWidget::itemSelectionChanged, this,
                     &ConnectionsPage::refreshButtonStates);
    QObject::connect(rememberedList_, &QListWidget::itemSelectionChanged, this, [this] {
        forgetButton_->setEnabled(rememberedList_->currentItem() != nullptr);
    });

    QObject::connect(model_->wifi(), &net::WifiConnectionManager::discoveredChanged, this,
                     &ConnectionsPage::rebuildLists);
    QObject::connect(model_->wifi(), &net::WifiConnectionManager::scanningChanged, this,
                     &ConnectionsPage::refreshButtonStates);
    // The Connect button's spinner needs to track LinkState transitions on
    // any remembered/known connection. Both ConnectionHub and the AppModel
    // republish state via stateChanged, but ConnectionHub::changed gives us
    // the lower-latency hook into mid-handshake LinkState moves.
    QObject::connect(model_->hub(), &net::ConnectionHub::changed, this,
                     &ConnectionsPage::rebuildLists);
    QObject::connect(model_, &AppModel::stateChanged, this, &ConnectionsPage::refreshButtonStates);

    rebuildLists();
    refreshButtonStates();
}

void ConnectionsPage::rebuildLists() {
    discoveredList_->clear();
    for (const auto& s : model_->wifi()->discoveredServers()) {
        auto* item = new QListWidgetItem(QStringLiteral("%1 • %2 • %3")
                                             .arg(s.name.isEmpty() ? s.ip : s.name, s.ip,
                                                  models::discoverySourceLabel(s.source)));
        item->setData(Qt::UserRole, QVariant::fromValue(s.id()));
        discoveredList_->addItem(item);
    }
    rememberedList_->clear();
    for (const auto& r : model_->wifi()->remembered()) {
        auto* conn = model_->wifi()->get(r.id);
        const QString liveTag = (conn != nullptr && conn->state() == net::SessionState::Live)
                                    ? QStringLiteral(" • online")
                                    : QString();
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 • %2%3").arg(r.name.isEmpty() ? r.ip : r.name, r.ip, liveTag));
        item->setData(Qt::UserRole, r.id);
        rememberedList_->addItem(item);
    }
    forgetButton_->setEnabled(rememberedList_->currentItem() != nullptr);
    refreshButtonStates();
}

bool ConnectionsPage::connectingForSelection() const {
    auto* item = discoveredList_->currentItem();
    if (item == nullptr) { return false; }
    const auto wantedId = item->data(Qt::UserRole).toString();
    // The discovered-list id is `ip:udpPort` (see DiscoveredServer::id());
    // remembered/known connections share the same id key, so a quick walk
    // through model.state().connections suffices.
    for (const auto& c : model_->state().connections) {
        if (c.id == wantedId && c.live == models::LinkState::Connecting) { return true; }
    }
    return false;
}

void ConnectionsPage::refreshButtonStates() {
    const bool scanning = model_->wifi()->isScanning();
    // Scan button: spinner inside the button, button disabled while in flight.
    // The disabled-state 40 % opacity in Theme.cpp carries the "not tappable"
    // signal so the button doesn't also need a separate text/colour swap.
    scanButton_->setEnabled(!scanning);
    scanButton_->setInFlight(scanning, QStringLiteral("Scanning…"), QStringLiteral("Scan"));
    statusLabel_->setText(scanning ? QStringLiteral("Scanning…") : QString());

    // Connect button: same pattern. We surface `Connecting…` while the
    // selected discovered row resolves to a known connection still in the
    // Connecting LinkState (= openSession's UDP handshake is running).
    const bool hasSelection = discoveredList_->currentItem() != nullptr;
    const bool connecting = connectingForSelection();
    connectButton_->setEnabled(hasSelection && !connecting);
    connectButton_->setInFlight(connecting, QStringLiteral("Connecting…"),
                                QStringLiteral("Connect"));
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
