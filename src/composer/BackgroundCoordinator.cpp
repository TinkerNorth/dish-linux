// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/BackgroundCoordinator.h"

#include "source/notification/DesktopNotifier.h"
#include "source/store/BackgroundPreferenceStore.h"
#include "source/tray/TrayIcon.h"

#include <QCoreApplication>

namespace dish::composer {

BackgroundCoordinator::BackgroundCoordinator(source::BackgroundPreferenceStore* prefs,
                                             source::TrayIcon* tray,
                                             source::DesktopNotifier* notifier, QObject* parent)
    : QObject(parent), prefs_(prefs), tray_(tray), notifier_(notifier) {
    if (tray_ == nullptr) { return; }
    QObject::connect(tray_, &source::TrayIcon::showWindowRequested, this,
                     &BackgroundCoordinator::showWindowRequested);
    QObject::connect(tray_, &source::TrayIcon::quitRequested, this,
                     &BackgroundCoordinator::quitRequested);
    QObject::connect(tray_, &source::TrayIcon::availabilityChanged, this,
                     &BackgroundCoordinator::onAvailabilityChanged);
}

bool BackgroundCoordinator::trayAvailable() const {
    return tray_ != nullptr && tray_->isAvailable();
}

reducer::CloseAction BackgroundCoordinator::closeRequested() {
    const bool enabled = prefs_ != nullptr && prefs_->runInBackground();
    const auto action = reducer::decideCloseAction(enabled, trayAvailable());
    const bool announced = prefs_ != nullptr && prefs_->noticeShown();
    if (reducer::shouldAnnounceBackground(action, announced)) {
        announce();
        if (prefs_ != nullptr) { prefs_->setNoticeShown(true); }
    }
    return action;
}

void BackgroundCoordinator::setWindowVisible(bool visible) { windowVisible_.set(visible); }

// A panel that dies while the window is hidden would otherwise leave a running
// Dish with nothing to click, so the window comes back instead.
void BackgroundCoordinator::onAvailabilityChanged(bool available) {
    emit trayAvailabilityChanged(available);
    if (!available && !windowVisible_.value()) { emit showWindowRequested(); }
}

// Composed here rather than in QML because there is no window left to read it
// in; this is the same sanctioned translate() seam the theme labels use.
void BackgroundCoordinator::announce() {
    if (notifier_ == nullptr) { return; }
    notifier_->notify(
        QCoreApplication::translate("dish::composer::BackgroundCoordinator", "Dish is still running"),
        QCoreApplication::translate("dish::composer::BackgroundCoordinator",
                                    "Controllers keep streaming. Quit from the tray icon."));
}

} // namespace dish::composer
