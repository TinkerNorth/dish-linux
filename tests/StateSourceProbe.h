// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Member order matters: the subscription is declared last so it tears down
// before the captured vector, and callbacks never fire into freed storage.

#pragma once

#include "architecture/Observable.h"

#include <cstddef>
#include <vector>

namespace dish::test {

template <class S> class StateSourceProbe {
  public:
    explicit StateSourceProbe(const dish::arch::Observable<S>& observable) {
        subscription_ =
            observable.subscribe([this](const S& value) { states_.push_back(value); }, true);
    }

    const std::vector<S>& states() const { return states_; }
    const S& latest() const { return states_.back(); }
    std::size_t count() const { return states_.size(); }

  private:
    std::vector<S> states_;
    typename dish::arch::Observable<S>::Subscription subscription_;
};

} // namespace dish::test
