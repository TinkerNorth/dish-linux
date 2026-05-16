// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "MainWindow.h"

#include "AppModel.h"
#include "ConnectionsPage.h"
#include "ErrorBanner.h"
#include "Network/ConnectionHub.h"
#include "Network/WifiConnection.h"
#include "Network/WifiConnectionManager.h"
#include "PairingPage.h"
#include "SettingsView.h"
#include "SlotCard.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace dish::ui {

MainWindow::MainWindow(AppModel* model, QWidget* parent) : QMainWindow(parent), model_(model) {
    setWindowTitle(QStringLiteral("Dish"));
    resize(520, 640);

    auto* central = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    setCentralWidget(central);

    stack_ = new QStackedWidget(central);
    centralLayout->addWidget(stack_, 1);

    errorBanner_ = new ErrorBanner(central);
    auto* bannerWrap = new QHBoxLayout;
    bannerWrap->setContentsMargins(20, 0, 20, 16);
    bannerWrap->addWidget(errorBanner_);
    centralLayout->addLayout(bannerWrap);

    dashboardPage_ = new QWidget(stack_);
    buildDashboardPage();
    stack_->addWidget(dashboardPage_);

    connectionsPage_ = new ConnectionsPage(model_, stack_);
    stack_->addWidget(connectionsPage_);

    pairingPage_ = new PairingPage(stack_);
    stack_->addWidget(pairingPage_);

    settingsPage_ = new SettingsView(model_->featureSettings(), stack_);
    stack_->addWidget(settingsPage_);

    QObject::connect(connectionsPage_, &ConnectionsPage::backRequested, this,
                     &MainWindow::showDashboard);
    QObject::connect(pairingPage_, &PairingPage::cancelRequested, this,
                     &MainWindow::returnFromPairing);
    QObject::connect(pairingPage_, &PairingPage::pairRequested, this, &MainWindow::onPairSubmit);
    // The settings page's Done button returns to the dashboard.
    QObject::connect(settingsPage_, &SettingsView::closeRequested, this,
                     &MainWindow::showDashboard);

    QObject::connect(model_, &AppModel::stateChanged, this, &MainWindow::onStateChanged);
    QObject::connect(model_, &AppModel::errorMessage, this, &MainWindow::onError);

    onStateChanged();
}

void MainWindow::buildDashboardPage() {
    auto* root = new QVBoxLayout(dashboardPage_);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(16);

    auto* headerRow = new QHBoxLayout;
    headerRow->setSpacing(10);
    statusDot_ = new QLabel(dashboardPage_);
    statusDot_->setFixedSize(8, 8);
    statusDot_->setStyleSheet(dotQss(Theme::muted));
    statusText_ = new QLabel(dashboardPage_);
    statusText_->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
    settingsButton_ = new QPushButton(QStringLiteral("Settings"), dashboardPage_);
    manageButton_ = new QPushButton(QStringLiteral("Manage"), dashboardPage_);
    headerRow->addWidget(statusDot_, 0, Qt::AlignVCenter);
    headerRow->addWidget(statusText_, 1, Qt::AlignVCenter);
    headerRow->addWidget(settingsButton_, 0, Qt::AlignVCenter);
    headerRow->addWidget(manageButton_, 0, Qt::AlignVCenter);

    summaryText_ = new QLabel(dashboardPage_);
    summaryText_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;").arg(hex(Theme::muted)));

    auto* headerBox = new QVBoxLayout;
    headerBox->setSpacing(6);
    headerBox->addLayout(headerRow);
    headerBox->addWidget(summaryText_);
    root->addLayout(headerBox);

    dashboardSpinner_ = new QProgressBar(dashboardPage_);
    dashboardSpinner_->setRange(0, 0);
    dashboardSpinner_->setTextVisible(false);
    dashboardSpinner_->setFixedHeight(4);
    auto dashSp = dashboardSpinner_->sizePolicy();
    dashSp.setRetainSizeWhenHidden(true);
    dashboardSpinner_->setSizePolicy(dashSp);
    dashboardSpinner_->setVisible(false);
    root->addWidget(dashboardSpinner_);

    auto* divider = new QFrame(dashboardPage_);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: %1;").arg(hex(Theme::outline)));
    root->addWidget(divider);

    auto* slotsHeader = new QLabel(QStringLiteral("CONTROLLERS"), dashboardPage_);
    slotsHeader->setStyleSheet(sectionHeaderQss());
    root->addWidget(slotsHeader);

    auto* scroll = new QScrollArea(dashboardPage_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* slotsContainer = new QWidget(scroll);
    slotsLayout_ = new QVBoxLayout(slotsContainer);
    slotsLayout_->setContentsMargins(0, 0, 0, 0);
    slotsLayout_->setSpacing(8);
    slotsEmpty_ = new QLabel(QStringLiteral("No controllers connected"), slotsContainer);
    slotsEmpty_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;").arg(hex(Theme::muted)));
    slotsLayout_->addWidget(slotsEmpty_);
    slotsLayout_->addStretch(1);
    scroll->setWidget(slotsContainer);
    root->addWidget(scroll, 1);

    QObject::connect(manageButton_, &QPushButton::clicked, this, &MainWindow::onManageClicked);
    QObject::connect(settingsButton_, &QPushButton::clicked, this, &MainWindow::onSettingsClicked);
}

void MainWindow::onStateChanged() {
    rebuildHeader();
    rebuildSlotList();
    dashboardSpinner_->setVisible(model_->state().busy);
    if (awaitingPair_) {
        for (const auto& c : model_->state().connections) {
            if (c.id == awaitingPairConnectionId_ && c.live == models::ConnectionLive::Connected) {
                awaitingPair_ = false;
                awaitingPairConnectionId_.clear();
                pairingPage_->setPending(false);
                returnFromPairing();
                break;
            }
        }
    }
    if (model_->state().pairingTarget.has_value()) { maybeShowPairingPage(); }
}

void MainWindow::rebuildHeader() {
    const auto& conns = model_->state().connections;
    int live = 0;
    QString firstLabel;
    for (const auto& c : conns) {
        if (c.live == models::ConnectionLive::Connected) {
            ++live;
            if (firstLabel.isEmpty()) { firstLabel = c.label; }
        }
    }
    const int total = static_cast<int>(conns.size());
    QString status;
    if (live == 0 && total == 0) {
        status = QStringLiteral("No connections yet");
    } else if (live == 0) {
        status = QStringLiteral("%1 remembered").arg(total);
    } else if (live == 1) {
        status = firstLabel;
    } else {
        status = QStringLiteral("%1 active connections").arg(live);
    }
    statusText_->setText(status);
    statusDot_->setStyleSheet(dotQss(live > 0 ? Theme::success : Theme::muted));
    statusText_->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600; color: %1;")
                                   .arg(hex(live > 0 ? Theme::success : Theme::muted)));

    QString summary;
    if (live == 0 && total == 0) {
        summary = QStringLiteral("Tap Manage to add one");
    } else if (live == 0) {
        summary = QStringLiteral("%1 remembered").arg(total);
    } else {
        summary = QStringLiteral("%1 of %2 connected").arg(live).arg(total);
    }
    summaryText_->setText(summary);
}

void MainWindow::rebuildSlotList() {
    // Note: Qt's `slots` keyword/macro precludes naming a local `slots`.
    const auto& slotItems = model_->state().slotList;
    const auto& conns = model_->state().connections;

    // Available connections for binding = those not bound to another slot.
    QList<models::ConnectionSummary> available;
    for (const auto& c : conns) {
        if (!c.boundSlotId.has_value()) { available.append(c); }
    }

    // Drop existing SlotCards (keep the trailing stretch + the empty label).
    for (int i = slotsLayout_->count() - 1; i >= 0; --i) {
        auto* item = slotsLayout_->itemAt(i);
        if (auto* w = item->widget()) {
            if (w == slotsEmpty_) { continue; }
            slotsLayout_->removeWidget(w);
            w->deleteLater();
        }
    }

    slotsEmpty_->setVisible(slotItems.isEmpty());

    // Re-insert before the trailing stretch.
    for (const auto& s : slotItems) {
        auto* card = new SlotCard(this);
        card->setSlot(s, available);
        QObject::connect(card, &SlotCard::bindRequested, this, &MainWindow::onBindRequested);
        QObject::connect(card, &SlotCard::unbindRequested, this, &MainWindow::onUnbindRequested);
        slotsLayout_->insertWidget(slotsLayout_->count() - 1, card);
    }
}

void MainWindow::maybeShowPairingPage() {
    auto target = model_->state().pairingTarget;
    if (!target.has_value()) { return; }
    pairingPage_->setServer(*target);
    model_->clearPairingTarget();
    auto* current = stack_->currentWidget();
    pairingReturnPage_ = (current == pairingPage_) ? dashboardPage_ : current;
    stack_->setCurrentWidget(pairingPage_);
}

void MainWindow::onError(const QString& msg) {
    if (awaitingPair_) {
        awaitingPair_ = false;
        awaitingPairConnectionId_.clear();
        pairingPage_->setPending(false);
    }
    errorBanner_->showError(msg);
}

void MainWindow::onManageClicked() { stack_->setCurrentWidget(connectionsPage_); }

void MainWindow::onSettingsClicked() { stack_->setCurrentWidget(settingsPage_); }

void MainWindow::onBindRequested(const QString& slotId, const QString& connectionId) {
    model_->hub()->bind(slotId, connectionId);
}

void MainWindow::onUnbindRequested(const QString& slotId) { model_->hub()->unbind(slotId); }

void MainWindow::showDashboard() { stack_->setCurrentWidget(dashboardPage_); }

void MainWindow::returnFromPairing() {
    awaitingPair_ = false;
    awaitingPairConnectionId_.clear();
    pairingPage_->setPending(false);
    stack_->setCurrentWidget(pairingReturnPage_ != nullptr ? pairingReturnPage_ : dashboardPage_);
    pairingReturnPage_ = nullptr;
}

void MainWindow::onPairSubmit(const models::DiscoveredServer& server, const QString& pin) {
    awaitingPair_ = true;
    awaitingPairConnectionId_ = net::WifiConnection::idFor(server);
    pairingPage_->setPending(true);
    model_->wifi()->pairWithPin(server, pin);
}

} // namespace dish::ui
