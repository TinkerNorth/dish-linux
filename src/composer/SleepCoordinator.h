// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Turns logind's suspend edge into connection commands. The effect sink is a
// seam so the reduction can be pinned without a bus or a live satellite.

#pragma once

#include "core/reducer/SleepCycle.h"

#include <QObject>

#include <functional>

namespace dish::source {
class SleepMonitor;
}

namespace dish::composer {

class ConnectionCoordinator;

class SleepCoordinator : public QObject {
    Q_OBJECT
  public:
    using EffectSink = std::function<void(reducer::SleepEffect)>;

    SleepCoordinator(source::SleepMonitor* monitor, ConnectionCoordinator* connections,
                     QObject* parent = nullptr);
    SleepCoordinator(source::SleepMonitor* monitor, EffectSink sink, QObject* parent = nullptr);
    ~SleepCoordinator() override = default;

    reducer::SleepPhase phase() const { return phase_; }

  private:
    void onPreparingForSleep(bool starting);

    EffectSink sink_;
    reducer::SleepPhase phase_ = reducer::SleepPhase::Awake;
};

} // namespace dish::composer
