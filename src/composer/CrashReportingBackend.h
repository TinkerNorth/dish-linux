// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The seam a crash-reporting opt-in flip is routed to.
//
// Dish collects crashes LOCALLY and transmits nothing, ever: CrashHandler writes
// a backtrace to $XDG_STATE_HOME/dish/crash.log, and UI/CrashReport turns that
// into a redacted report the user reads and sends themselves. There is no
// upload path to wire and no account to hold.
//
// The backend below therefore has nothing to do but record the flip. It stays as
// an interface because the local writer is armed before the preference is known
// — see CrashReportingController — and because a future opt-in uploader, if one
// is ever wanted, belongs here rather than threaded through the UI.

#pragma once

namespace dish::composer {

class CrashReportingBackend {
  public:
    virtual ~CrashReportingBackend() = default;
    virtual void setEnabled(bool enabled) = 0;
};

// Records the flip to the debug log and does nothing else. Named for what the
// product does, not for the pattern: crashes ARE collected, on this machine.
class LocalCrashReportingBackend : public CrashReportingBackend {
  public:
    void setEnabled(bool enabled) override;
};

} // namespace dish::composer
