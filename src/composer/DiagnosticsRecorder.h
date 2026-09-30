// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Feeds the flight recorder by diffing what the UI already renders: the
// connection rows each time they republish, and the slot list after each
// rebuild. It reads nothing the input path touches, and its first pass is the
// session's opening inventory.

#pragma once

#include "Models/Models.h"
#include "architecture/Observable.h"
#include "composer/ConnectionsComposer.h"
#include "core/reducer/DiagnosticsLog.h"
#include "source/store/DiagnosticsLogStore.h"

#include <QList>

#include <cstdint>
#include <functional>
#include <vector>

namespace dish::composer {

// The two snapshots the diff reads, taken from the shapes the UI renders.
reducer::LinkSnapshot linkSnapshotOf(const ConnectionRow& row);
reducer::PadSnapshot padSnapshotOf(const models::ControllerSlot& slot);

class DiagnosticsRecorder {
  public:
    // Wall-clock milliseconds, so a copied log lines up with the user's own
    // account of when something happened.
    using Clock = std::function<std::int64_t()>;

    DiagnosticsRecorder(const arch::Observable<std::vector<ConnectionRow>>& links,
                        source::DiagnosticsLogStore* log, Clock clock);

    void observeSlots(const QList<models::ControllerSlot>& slotList);

  private:
    void observeLinks(const std::vector<ConnectionRow>& rows);

    source::DiagnosticsLogStore* log_;
    Clock clock_;
    std::vector<reducer::LinkSnapshot> links_;
    std::vector<reducer::PadSnapshot> pads_;
    // Declared last: subscribing replays the current rows into observeLinks,
    // which reads every member above.
    arch::Observable<std::vector<ConnectionRow>>::Subscription linksSub_;
};

} // namespace dish::composer
