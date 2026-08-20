// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Routes each crash-reporting opt-in flip from the store to the backend seam.
// The one behaviour that sets this Controller apart: stop() is a deliberate
// no-op, so a flip after teardown still reaches the backend.

#pragma once

#include "architecture/Controller.h"
#include "composer/CrashReportingBackend.h"
#include "source/store/CrashReportingStore.h"

namespace dish::composer {

class CrashReportingController : public arch::Controller<bool> {
  public:
    // `backend` is borrowed — owned by the composition root.
    CrashReportingController(const arch::Observable<bool>& enabled, CrashReportingBackend* backend);

  protected:
    void apply(const bool& enabled) override;

  public:
    // Deliberate no-op: do NOT cancel the subscription. Process-scoped, so the
    // opt-in keeps propagating across teardowns.
    void stop() override;

  private:
    CrashReportingBackend* backend_;
};

} // namespace dish::composer
