// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The pad level of Diagnostics (docs/controller-db.md section 9): after each
// test the person says whether it worked, and the answer is kept as an
// outcome, `{ test, result, atMs }`, per controller, the last twenty, oldest
// first, in memory for the run. The bundle carries them. The tests and their
// names are the bench's (core/reducer/BenchItems.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dish::diagnostics {

inline constexpr std::size_t kOutcomesKept = 20;

struct TestOutcome {
    std::string test;
    std::string result;
    std::int64_t atMs = 0;

    bool operator==(const TestOutcome&) const = default;
};

using OutcomeLog = std::vector<TestOutcome>;

inline OutcomeLog appendOutcome(OutcomeLog log, TestOutcome outcome) {
    log.push_back(std::move(outcome));
    if (log.size() > kOutcomesKept) {
        const auto excess = static_cast<std::ptrdiff_t>(log.size() - kOutcomesKept);
        log.erase(log.begin(), log.begin() + excess);
    }
    return log;
}

inline std::string_view outcomeResult(bool worked) { return worked ? "worked" : "didNotWork"; }

} // namespace dish::diagnostics
