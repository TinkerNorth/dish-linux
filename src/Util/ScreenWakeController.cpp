// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ScreenWakeController.h"

namespace dish::util {

ScreenWakeController::ScreenWakeController(DisplaySleepInhibitor* inhibitor, QString reason)
    : inhibitor_(inhibitor), reason_(std::move(reason)) {}

int ScreenWakeController::streamingCount(
    const QHash<QString, QString>& bindings,
    const QHash<QString, models::ConnectionLive>& connectionStates) {
    int count = 0;
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        if (connectionStates.value(it.value(), models::ConnectionLive::Idle) ==
            models::ConnectionLive::Connected) {
            ++count;
        }
    }
    return count;
}

void ScreenWakeController::update(int streamingSlotCount) {
    const int was = streamingSlotCount_;
    streamingSlotCount_ = streamingSlotCount;
    if (was == 0 && streamingSlotCount > 0) {
        if (inhibitor_ != nullptr) { inhibitor_->acquire(reason_); }
    } else if (was > 0 && streamingSlotCount == 0) {
        if (inhibitor_ != nullptr) { inhibitor_->release(); }
    }
}

void ScreenWakeController::reset() {
    streamingSlotCount_ = 0;
    if (inhibitor_ != nullptr) { inhibitor_->release(); }
}

} // namespace dish::util
