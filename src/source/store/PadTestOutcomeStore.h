// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// PadTestOutcomeStore — the answers to "Did it work?" after each pad test, per
// slot, the last twenty, oldest first (core/diagnostics/PadTestOutcomes.h).
// In memory for the run, as the contract has it: nothing here is persisted,
// and the diagnostics bundle is the only place they ever leave to.

#pragma once

#include "architecture/StateSource.h"
#include "core/diagnostics/PadTestOutcomes.h"

#include <map>
#include <optional>
#include <string>
#include <utility>

namespace dish::source {

using PadTestOutcomeMap = std::map<std::string, diagnostics::OutcomeLog>;

class PadTestOutcomeStore : public arch::StateSource<PadTestOutcomeMap> {
  public:
    PadTestOutcomeStore() : arch::StateSource<PadTestOutcomeMap>(PadTestOutcomeMap{}) {}

    void record(const std::string& slotId, diagnostics::TestOutcome outcome) {
        PadTestOutcomeMap next = state().value();
        next[slotId] = diagnostics::appendOutcome(std::move(next[slotId]), std::move(outcome));
        setState(std::move(next));
    }

    diagnostics::OutcomeLog outcomesFor(const std::string& slotId) const {
        const PadTestOutcomeMap all = state().value();
        const auto it = all.find(slotId);
        return it == all.end() ? diagnostics::OutcomeLog{} : it->second;
    }

    std::optional<diagnostics::TestOutcome> lastFor(const std::string& slotId) const {
        const diagnostics::OutcomeLog log = outcomesFor(slotId);
        if (log.empty()) { return std::nullopt; }
        return log.back();
    }
};

} // namespace dish::source
