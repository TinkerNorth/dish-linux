// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/SleepCoordinator.h"

#include "composer/ConnectionCoordinator.h"
#include "source/system/SleepMonitor.h"

#include <utility>

namespace dish::composer {

SleepCoordinator::SleepCoordinator(source::SleepMonitor* monitor,
                                   ConnectionCoordinator* connections, QObject* parent)
    : SleepCoordinator(
          monitor,
          [connections](reducer::SleepEffect effect) {
              if (connections == nullptr) { return; }
              switch (effect) {
              case reducer::SleepEffect::None:
                  break;
              case reducer::SleepEffect::TearDown:
                  connections->prepareForSleep();
                  break;
              case reducer::SleepEffect::Reconnect:
                  connections->resumeFromSleep();
                  break;
              }
          },
          parent) {}

SleepCoordinator::SleepCoordinator(source::SleepMonitor* monitor, EffectSink sink, QObject* parent)
    : QObject(parent), sink_(std::move(sink)) {
    if (monitor == nullptr) { return; }
    QObject::connect(monitor, &source::SleepMonitor::preparingForSleep, this,
                     &SleepCoordinator::onPreparingForSleep);
}

void SleepCoordinator::onPreparingForSleep(bool starting) {
    const auto reduction = reducer::reduceSleepCycle(phase_, starting);
    phase_ = reduction.next;
    if (reduction.effect == reducer::SleepEffect::None) { return; }
    if (sink_) { sink_(reduction.effect); }
}

} // namespace dish::composer
