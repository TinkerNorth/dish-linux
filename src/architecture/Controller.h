// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Controller<S>: side-effect actuator, runs apply() on each upstream value.
// See architecture/README.md.

#pragma once

#include "architecture/Observable.h"

namespace dish::arch {

template <class S> class Controller {
  public:
    explicit Controller(const Observable<S>& upstream) : upstream_(&upstream) {}
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    virtual ~Controller() = default;

    void start() {
        if (started_) { return; }
        started_ = true;
        onStarting();
        subscription_ = upstream_->subscribe([this](const S& value) { apply(value); },
                                             /*emitCurrent=*/true);
    }

    virtual void stop() { cancelCollection(); }

  protected:
    virtual void apply(const S& value) = 0;

    // Hook for re-arming state a previous stop() tore down.
    virtual void onStarting() {}

    void cancelCollection() {
        subscription_.reset();
        started_ = false;
    }

    const Observable<S>& upstream() const { return *upstream_; }

  private:
    const Observable<S>* upstream_;
    typename Observable<S>::Subscription subscription_;
    bool started_ = false;
};

} // namespace dish::arch
