// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Drives core/reducer/UpdateMachine over the manifest gateway and a schedule.
//
// Dish never installs its own update on Linux: a distro package or Flatpak
// owns the binary, and a self-applying updater would fight the package
// manager. So the machine runs with notifyOnly set and the reachable phases
// stop at Available — the UI surfaces the release and links out. The
// download, verify and staging arms of the reducer are unreachable here by
// construction, which is why there is no staging store and no boot gate.

#pragma once

#include "architecture/Observable.h"
#include "core/reducer/UpdateMachine.h"
#include "source/store/UpdatePreferenceStore.h"
#include "update/UpdatePorts.h"

#include <QDateTime>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QTimer;

namespace dish::update {

class UpdateChecker : public QObject {
    Q_OBJECT
  public:
    explicit UpdateChecker(source::UpdatePreferenceStore* prefs, QObject* parent = nullptr);

    // Tests: the supplied gateway runs on the CALLING thread and every callback
    // is delivered inline, so a whole check cycle is assertable without an
    // event loop.
    UpdateChecker(source::UpdatePreferenceStore* prefs, std::unique_ptr<ManifestGateway> gateway,
                  QObject* parent = nullptr);

    ~UpdateChecker() override;

    // The one reactive surface. AppViewModel subscribes; nothing else may.
    const arch::Observable<reducer::UpdateStatus>& status() const { return status_; }
    reducer::UpdateStatus snapshot() const { return status_.value(); }

    // Arms the startup schedule. Safe to call once; a second call is ignored.
    void start();

    // Bypasses the interval and the backoff, rate-limited to one per 10 s so a
    // held-down button cannot hammer the permalink.
    void checkNow();

    // Mutes status().availableVersion until the manifest offers a newer one.
    // Ignored while the running build is below the supported minimum.
    void skipAvailableVersion();

    // Invalid when no check has ever completed.
    QDateTime lastCheck() const;

    // The armed check delay in ms, or -1 when none is scheduled. With
    // firePendingCheck() this is the seam the schedule tests drive.
    int pendingCheckDelayMs() const { return pendingCheckDelayMs_; }
    void firePendingCheck();

    // Milliseconds since the Unix epoch, UTC. Injected so the minimum gap is
    // testable without touching the clock.
    using ClockFn = std::function<qint64()>;
    void setClock(ClockFn clock);

    // The running build, DISH_VERSION unless a test overrides it before start().
    void setCurrentVersion(const QString& version);

  signals:
    // Edge-detected, at most once per version per session. The facade maps the
    // enum to a token; the engine never vends a sentence.
    void notice(dish::reducer::UpdateNotice notice, const QString& version);

  private:
    void construct();
    void dispatch(const reducer::UpdateEvent& event);
    void execute(const reducer::UpdateEffect& effect);
    void onPrefsChanged(const source::UpdatePreferences& prefs);
    void persistLastCheck();

    source::UpdatePreferenceStore* prefs_ = nullptr;
    std::unique_ptr<ManifestGateway> gateway_;
    arch::Observable<reducer::UpdateStatus> status_{reducer::UpdateStatus{}};
    arch::Observable<source::UpdatePreferences>::Subscription prefsSub_;

    QTimer* checkTimer_ = nullptr;
    int pendingCheckDelayMs_ = -1;
    qint64 lastManualCheckMs_ = 0;
    bool started_ = false;
    ClockFn clock_;
};

} // namespace dish::update
