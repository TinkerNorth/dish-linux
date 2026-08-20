// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The seam a crash-reporting opt-in flip is routed to. The shipped
// implementation is a no-op: nothing is collected and nothing is transmitted.
// Wiring a real backend means replacing NoopCrashReportingBackend below — the
// store, the controller and the user-facing toggle already carry the opt-in.

#pragma once

namespace dish::composer {

class CrashReportingBackend {
  public:
    virtual ~CrashReportingBackend() = default;
    virtual void setEnabled(bool enabled) = 0;
};

// Records the flip to the debug log and does nothing else.
class NoopCrashReportingBackend : public CrashReportingBackend {
  public:
    void setEnabled(bool enabled) override;
};

} // namespace dish::composer
