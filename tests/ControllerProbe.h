// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

namespace dish::test {

template <class C> class ControllerProbe {
  public:
    explicit ControllerProbe(C& controller) : controller_(controller) {}

    void start() { controller_.start(); }
    void stop() { controller_.stop(); }

  private:
    C& controller_;
};

} // namespace dish::test
