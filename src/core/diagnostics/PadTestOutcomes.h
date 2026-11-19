// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The pad level of Diagnostics (docs/controller-db.md section 9): after each
// test the person says whether it worked, and the answer is kept as an
// outcome, `{ test, result, atMs }`, per controller, the last twenty, oldest
// first, in memory for the run. The bundle carries them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

// The bench's motor words name the test: "weak" is "rumble.weak". Nothing else is a test.
inline std::optional<std::string> rumbleTestName(std::string_view motor) {
    const bool known = motor == "weak" || motor == "strong" || motor == "both";
    if (!known) { return std::nullopt; }
    return "rumble." + std::string(motor);
}

inline std::string_view outcomeResult(bool worked) { return worked ? "worked" : "didNotWork"; }

} // namespace dish::diagnostics
