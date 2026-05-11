// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "MainWindow.h"

#include "AppModel.h"
#include "ConnectionsDialog.h"
#include "Network/ConnectionHub.h"
#include "Network/WifiConnectionManager.h"
#include "PairingDialog.h"
#include "SlotCard.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace dish::ui {

MainWindow::MainWindow(AppModel* model, QWidget* parent) : QMainWindow(parent), model_(model) {
    setWindowTitle(QStringLiteral("Dish"));
    resize(520, 640);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(16);

    // Header --------------------------------------------------------------
    auto* headerRow = new QHBoxLayout;
    headerRow->setSpacing(10);
    statusDot_ = new QLabel(central);
    statusDot_->setFixedSize(8, 8);
    statusDot_->setStyleSheet(dotQss(Theme::muted));
    statusText_ = new QLabel(central);
    statusText_->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
    manageButton_ = new QPushButton(QStringLiteral("Manage"), central);
    headerRow->addWidget(statusDot_, 0, Qt::AlignVCenter);
    headerRow->addWidget(statusText_, 1, Qt::AlignVCenter);
    headerRow->addWidget(manageButton_, 0, Qt::AlignVCenter);

    summaryText_ = new QLabel(central);
    summaryText_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;").arg(hex(Theme::muted)));

    auto* headerBox = new QVBoxLayout;
    headerBox->setSpacing(6);
    headerBox->addLayout(headerRow);
    headerBox->addWidget(summaryText_);
    root->addLayout(headerBox);

    auto* divider = new QFrame(central);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: %1;").arg(hex(Theme::outline)));
    root->addWidget(divider);

    // Controllers section -------------------------------------------------
    auto* slotsHeader = new QLabel(QStringLiteral("CONTROLLERS"), central);
    slotsHeader->setStyleSheet(sectionHeaderQss());
    root->addWidget(slotsHeader);

    auto* scroll = new QScrollArea(central);
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

    // Telemetry footer ----------------------------------------------------
    auto* footer = new QHBoxLayout;
    telemetryLeft_ = new QLabel(central);
    telemetryRight_ = new QLabel(central);
    const QString footerStyle =
        QStringLiteral("color: %1; font-family: monospace; font-size: 10px;")
            .arg(hex(Theme::muted));
    telemetryLeft_->setStyleSheet(footerStyle);
    telemetryRight_->setStyleSheet(footerStyle);
    footer->addWidget(telemetryLeft_);
    footer->addStretch(1);
    footer->addWidget(telemetryRight_);
    root->addLayout(footer);

    setCentralWidget(central);

    QObject::connect(manageButton_, &QPushButton::clicked, this, &MainWindow::onManageClicked);
    // Single observer on the canonical state slice — rebuild header + slot list
    // and react to any pending pairing prompt every time state changes.
    QObject::connect(model_, &AppModel::stateChanged, this, &MainWindow::onStateChanged);
    QObject::connect(model_, &AppModel::errorMessage, this, &MainWindow::onError);

    telemetryTimer_ = new QTimer(this);
    telemetryTimer_->setInterval(1'000);
    QObject::connect(telemetryTimer_, &QTimer::timeout, this, &MainWindow::onTelemetryTick);
    telemetryTimer_->start();

    onStateChanged();
    onTelemetryTick();
}

void MainWindow::onStateChanged() {
    rebuildHeader();
    rebuildSlotList();
    if (model_->state().pairingTarget.has_value()) { showPairingPrompt(); }
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

void MainWindow::showPairingPrompt() {
    auto target = model_->state().pairingTarget;
    if (!target.has_value()) { return; }
    PairingDialog dlg(*target, this);
    const auto server = *target;
    model_->clearPairingTarget();
    if (dlg.exec() == QDialog::Accepted) { model_->wifi()->pairWithPin(server, dlg.pin()); }
}

void MainWindow::onError(const QString& msg) {
    QMessageBox::warning(this, QStringLiteral("Error"), msg);
}

void MainWindow::onTelemetryTick() {
    auto snap = model_->processor()->drainTelemetry();
    telemetryTotal_ = snap.totalSent;
    telemetryLeft_->setText(
        QStringLiteral("events/s %1   sends/s %2").arg(snap.events).arg(snap.sends));
    telemetryRight_->setText(QStringLiteral("total %1").arg(telemetryTotal_));
}

void MainWindow::onManageClicked() {
    ConnectionsDialog dlg(model_, this);
    dlg.exec();
}

void MainWindow::onBindRequested(const QString& slotId, const QString& connectionId) {
    model_->hub()->bind(slotId, connectionId);
}

void MainWindow::onUnbindRequested(const QString& slotId) { model_->hub()->unbind(slotId); }

} // namespace dish::ui
