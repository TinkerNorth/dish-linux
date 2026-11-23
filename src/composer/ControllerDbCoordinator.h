// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// ControllerDbCoordinator — runs the controller database check
// (docs/controller-db.md section 6.3): ten seconds after start and every six
// hours after, and once on "Check now". The manifest with If-None-Match; on a
// 304 or a version no newer than the one in force, done; otherwise each file
// the manifest names, one after the other, each checked against its SHA-256
// and size, then all of them parsed and checked for schema 1, and only then
// committed. A failure at any step keeps the database in force and is
// recorded as the check's result. The switch off means no request at all,
// "Check now" included, and a check in flight when it flips is dropped.
//
// Every decision is core/controllerdb/DbFetch.h's; this class only sequences
// them over the port and the store. Main thread.

#pragma once

#include "architecture/Observable.h"
#include "core/controllerdb/ControllerRecord.h"
#include "core/controllerdb/DbFetch.h"
#include "source/controllerdb/ControllerDbGateway.h"
#include "source/controllerdb/ControllerDbStore.h"
#include "source/store/ControllerDbPreferenceStore.h"

#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class QTimer;

namespace dish::composer {

class ControllerDbCoordinator : public QObject {
    Q_OBJECT
  public:
    using Clock = std::function<std::int64_t()>;

    ControllerDbCoordinator(source::controllerdb::ControllerDbStore* store,
                            source::ControllerDbPreferenceStore* prefs,
                            source::controllerdb::ControllerDbPort* port, Clock wallClockMs,
                            QObject* parent = nullptr);
    ~ControllerDbCoordinator() override;

    // Arms the first check, ten seconds out.
    void start();

    // "Check now": one check regardless of the schedule, never regardless of the switch.
    void checkNow();
    // What the timers call.
    void scheduledCheck();

    bool inFlight() const { return inFlight_; }

  private:
    void run(dish::controllerdb::CheckTrigger trigger);
    void onManifest(const source::controllerdb::DbReply& reply);
    void fetchNextFile();
    void onFile(const source::controllerdb::DbReply& reply);
    void commitDownload();
    void finish(dish::controllerdb::CheckResult result);
    void onPreferences(const source::ControllerDbPreferences& prefs);

    source::controllerdb::ControllerDbStore* store_;
    source::ControllerDbPreferenceStore* prefs_;
    source::controllerdb::ControllerDbPort* port_;
    Clock wallClockMs_;
    QTimer* firstTimer_;
    QTimer* intervalTimer_;

    bool inFlight_ = false;
    // The manifest's files this build fetches, in its order (filesThisBuildFetches).
    std::vector<dish::controllerdb::ManifestFile> toFetch_;
    source::controllerdb::DbFiles download_;
    QByteArray manifestEtag_;
    std::size_t nextFile_ = 0;

    arch::Observable<source::ControllerDbPreferences>::Subscription prefsSub_;
};

} // namespace dish::composer
