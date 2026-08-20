// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The command surface for running without a window: what a close means, the
// one-time notice that says so, and the tray's two commands on the way back.
//
// Window visibility is published here rather than read from the shell, because
// the tray item is derived from it and the shell is the only thing that knows.

#pragma once

#include "architecture/Observable.h"
#include "core/reducer/BackgroundMode.h"

#include <QObject>

namespace dish::source {
class BackgroundPreferenceStore;
class DesktopNotifier;
class TrayIcon;
} // namespace dish::source

namespace dish::composer {

class BackgroundCoordinator : public QObject {
    Q_OBJECT
  public:
    BackgroundCoordinator(source::BackgroundPreferenceStore* prefs, source::TrayIcon* tray,
                          source::DesktopNotifier* notifier, QObject* parent = nullptr);
    ~BackgroundCoordinator() override = default;

    // Decides, and announces the first hide. Not const: the notice is one-shot
    // and answering the question is what spends it.
    reducer::WindowCloseAction closeRequested();

    bool trayAvailable() const;

    void setWindowVisible(bool visible);
    const arch::Observable<bool>& windowVisible() const { return windowVisible_; }

  signals:
    void showWindowRequested();
    void quitRequested();
    void trayAvailabilityChanged(bool available);

  private:
    void onAvailabilityChanged(bool available);
    void announce();

    source::BackgroundPreferenceStore* prefs_;
    source::TrayIcon* tray_;
    source::DesktopNotifier* notifier_;
    arch::Observable<bool> windowVisible_{true};
};

} // namespace dish::composer
