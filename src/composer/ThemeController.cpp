// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/ThemeController.h"

#include <utility>

namespace dish::composer {

ThemeController::ThemeController(const arch::Observable<source::ThemeMode>& mode)
    : ThemeController(
          mode, [] { return ui::detectSystemAppearance(); },
          [](ui::Appearance appearance) { ui::setActiveAppearance(appearance); }) {}

ThemeController::ThemeController(const arch::Observable<source::ThemeMode>& mode,
                                 SystemReader systemReader, ApplySink applySink)
    : arch::Controller<source::ThemeMode>(mode), systemReader_(std::move(systemReader)),
      applySink_(std::move(applySink)) {}

ui::Appearance ThemeController::resolve(source::ThemeMode mode) const {
    switch (mode) {
    case source::ThemeMode::Light:
        return ui::Appearance::Light;
    case source::ThemeMode::Dark:
        return ui::Appearance::Dark;
    case source::ThemeMode::System:
        break;
    }
    return systemReader_ ? systemReader_() : ui::Appearance::Dark;
}

void ThemeController::apply(const source::ThemeMode& mode) {
    const ui::Appearance appearance = resolve(mode);
    if (applySink_) { applySink_(appearance); }
}

} // namespace dish::composer
