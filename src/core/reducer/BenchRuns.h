// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One answer per run (docs/diagnostics-bundle.md, "The bench", rule 4): a pad
// runs one bench test at a time, the page asks whether it worked once it has
// ended, and the answer becomes the outcome, stamped with the time the test
// ended. A test started while another runs or waits for its answer replaces
// it, and the replaced run is never asked about.

#pragma once

#include "core/diagnostics/PadTestOutcomes.h"
#include "core/reducer/BenchItems.h"

#include <cstdint>
#include <optional>
#include <string>

namespace dish::reducer {

struct BenchRun {
    // The latest run's: a step or an end scheduled for an earlier run finds it moved on.
    std::uint64_t ticket = 0;
    std::optional<BenchTest> running;
    // The ended test whose answer is awaited, and when it ended.
    std::optional<BenchTest> asking;
    std::int64_t endedAtMs = 0;

    bool operator==(const BenchRun&) const = default;
};

inline BenchRun startedRun(const BenchRun& run, BenchTest test) {
    return BenchRun{run.ticket + 1, test, std::nullopt, 0};
}

inline bool isCurrentRun(const BenchRun& run, std::uint64_t ticket) {
    return ticket == run.ticket && run.running.has_value();
}

// The run's end, when `ticket` is still the running one's; anything else leaves the run as it is.
inline BenchRun endedRun(const BenchRun& run, std::uint64_t ticket, std::int64_t nowMs) {
    if (!isCurrentRun(run, ticket)) { return run; }
    return BenchRun{run.ticket, std::nullopt, run.running, nowMs};
}

struct BenchAnswer {
    BenchRun run;
    std::optional<diagnostics::TestOutcome> outcome;
};

// The person's answer to the ended test; nothing while none is awaited.
inline BenchAnswer answeredRun(const BenchRun& run, bool worked) {
    if (!run.asking.has_value()) { return BenchAnswer{run, std::nullopt}; }
    const diagnostics::TestOutcome outcome{std::string(benchTestName(*run.asking)),
                                           std::string(diagnostics::outcomeResult(worked)),
                                           run.endedAtMs};
    return BenchAnswer{BenchRun{run.ticket, run.running, std::nullopt, 0}, outcome};
}

} // namespace dish::reducer
