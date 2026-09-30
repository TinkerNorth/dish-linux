// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The flight recorder's lines, oldest first and bounded, in memory only: they
// describe this run and nothing about them is worth keeping past it. Fed by
// composer/DiagnosticsRecorder, never from the input path.

#pragma once

#include "architecture/StateSource.h"
#include "core/reducer/DiagnosticsLog.h"

#include <vector>

namespace dish::source {

class DiagnosticsLogStore : public arch::StateSource<std::vector<reducer::DiagnosticsEvent>> {
  public:
    DiagnosticsLogStore() : StateSource({}) {}

    void record(const std::vector<reducer::DiagnosticsEvent>& events) {
        if (events.empty()) { return; }
        setState(reducer::appendBounded(state().value(), events, reducer::kDiagnosticsLogCapacity));
    }
};

} // namespace dish::source
