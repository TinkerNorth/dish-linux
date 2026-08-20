// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Resolves the stored theme mode (System included) to a concrete Appearance and
// applies the palette. The two std::function seams exist so this is testable
// without touching the real registry or a live QGuiApplication.

#pragma once

#include "architecture/Controller.h"
#include "source/store/ThemePreferenceStore.h"
#include "UI/Theme.h"

#include <functional>

namespace dish::composer {

class ThemeController : public arch::Controller<source::ThemeMode> {
  public:
    // Resolves ThemeMode::System to a concrete Appearance (Dark/Light).
    using SystemReader = std::function<ui::Appearance()>;
    // Applies a resolved Appearance (set the active palette + re-theme).
    using ApplySink = std::function<void(ui::Appearance)>;

    // Production ctor: the real OS reader plus a live palette swap.
    explicit ThemeController(const arch::Observable<source::ThemeMode>& mode);

    // Test ctor: inject both seams.
    ThemeController(const arch::Observable<source::ThemeMode>& mode, SystemReader systemReader,
                    ApplySink applySink);

    ui::Appearance resolve(source::ThemeMode mode) const;

  protected:
    void apply(const source::ThemeMode& mode) override;

  private:
    SystemReader systemReader_;
    ApplySink applySink_;
};

} // namespace dish::composer
