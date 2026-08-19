// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "UI/Theme.h"
#include "architecture/Observable.h"
#include "composer/ThemeController.h"
#include "source/store/ThemePreferenceStore.h"

#include "ControllerProbe.h"
#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <vector>

using dish::arch::Observable;
using dish::composer::ThemeController;
using dish::source::ThemeMode;
using dish::source::themeModeFromStorage;
using dish::source::themeModeLabel;
using dish::source::themeModeToStorage;
using dish::source::ThemePreferenceStore;
using dish::test::ControllerProbe;
using dish::test::StateSourceProbe;
using dish::ui::Appearance;
using dish::ui::darkPalette;
using dish::ui::lightPalette;
using dish::ui::paletteFor;
using dish::ui::ThemePalette;

namespace {

std::unique_ptr<ThemePreferenceStore> makeStore(const QTemporaryDir& dir) {
    const QString path = dir.filePath(QStringLiteral("theme.ini"));
    auto settings = std::make_unique<QSettings>(path, QSettings::IniFormat);
    return std::make_unique<ThemePreferenceStore>(std::move(settings));
}

} // namespace

TEST_CASE("themeMode storage round-trips for all three modes", "[settings][theme]") {
    REQUIRE(themeModeFromStorage(themeModeToStorage(ThemeMode::System)) == ThemeMode::System);
    REQUIRE(themeModeFromStorage(themeModeToStorage(ThemeMode::Light)) == ThemeMode::Light);
    REQUIRE(themeModeFromStorage(themeModeToStorage(ThemeMode::Dark)) == ThemeMode::Dark);
}

TEST_CASE("themeMode storage uses the exact android tokens", "[settings][theme]") {
    // Schema pin: these strings are cross-client; renaming is a migration.
    REQUIRE(themeModeToStorage(ThemeMode::System) == QStringLiteral("system"));
    REQUIRE(themeModeToStorage(ThemeMode::Light) == QStringLiteral("light"));
    REQUIRE(themeModeToStorage(ThemeMode::Dark) == QStringLiteral("dark"));
}

TEST_CASE("themeModeFromStorage is lenient -- unknown values fall back to System",
          "[settings][theme]") {
    REQUIRE(themeModeFromStorage(QString()) == ThemeMode::System);
    REQUIRE(themeModeFromStorage(QStringLiteral("")) == ThemeMode::System);
    REQUIRE(themeModeFromStorage(QStringLiteral("garbage")) == ThemeMode::System);
    REQUIRE(themeModeFromStorage(QStringLiteral("oled-black-2027")) == ThemeMode::System);
    // Case-exact only.
    REQUIRE(themeModeFromStorage(QStringLiteral("Light")) == ThemeMode::System);
    REQUIRE(themeModeFromStorage(QStringLiteral("light")) == ThemeMode::Light);
}

TEST_CASE("themeModeLabel returns the locale-aware strings", "[settings][theme]") {
    constexpr const char* ctx = "dish::source::ThemePreferenceStore";
    REQUIRE(themeModeLabel(ThemeMode::Light) == QCoreApplication::translate(ctx, "Light"));
    REQUIRE(themeModeLabel(ThemeMode::Dark) == QCoreApplication::translate(ctx, "Dark"));
    REQUIRE(themeModeLabel(ThemeMode::System) == QCoreApplication::translate(ctx, "System"));
}

TEST_CASE("ThemePreferenceStore defaults to System on a fresh store", "[settings][theme]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto store = makeStore(dir);
    REQUIRE(store->mode() == ThemeMode::System);
}

TEST_CASE("ThemePreferenceStore persists setMode across a relaunch", "[settings][theme]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    {
        auto store = makeStore(dir);
        store->setMode(ThemeMode::Light);
    }
    {
        auto reloaded = makeStore(dir);
        REQUIRE(reloaded->mode() == ThemeMode::Light);
    }
}

TEST_CASE("ThemePreferenceStore emits only on an actual transition", "[settings][theme]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto store = makeStore(dir); // default System

    StateSourceProbe<ThemeMode> probe(store->state());
    REQUIRE(probe.count() == 1); // replayed current

    store->setMode(ThemeMode::System);
    REQUIRE(probe.count() == 1);

    store->setMode(ThemeMode::Dark);
    REQUIRE(probe.count() == 2);

    store->setMode(ThemeMode::Dark);
    REQUIRE(probe.count() == 2);
    REQUIRE(probe.latest() == ThemeMode::Dark);
}

TEST_CASE("light palette defines every dark token role and differs from dark",
          "[settings][theme]") {
    const ThemePalette& dark = darkPalette();
    const ThemePalette& light = lightPalette();

    // A forgotten or zeroed light token shows up as a non-opaque role.
    const auto opaque = [](QRgb c) { return qAlpha(c) == 0xFF; };
    REQUIRE(opaque(dark.pulse));
    REQUIRE(opaque(light.pulse));
    REQUIRE(light.pulse != dark.pulse);
    REQUIRE(qGray(light.pulse) < qGray(dark.pulse));
    for (QRgb c : {dark.background, dark.surface, dark.surfaceDim, dark.primary, dark.primaryDark,
                   dark.onPrimary, dark.onSurface, dark.muted, dark.success, dark.error,
                   dark.warning, dark.glyph, dark.disabledFg, dark.mutedStrong}) {
        REQUIRE(opaque(c));
    }
    for (QRgb c : {light.background, light.surface, light.surfaceDim, light.primary,
                   light.primaryDark, light.onPrimary, light.onSurface, light.muted, light.success,
                   light.error, light.warning, light.glyph, light.disabledFg, light.mutedStrong}) {
        REQUIRE(opaque(c));
    }

    REQUIRE(light.background != dark.background);
    REQUIRE(light.surface != dark.surface);
    REQUIRE(light.surfaceDim != dark.surfaceDim);
    REQUIRE(light.onSurface != dark.onSurface);
    REQUIRE(light.primary != dark.primary);
    REQUIRE(light.muted != dark.muted);
    REQUIRE(light.glyph != dark.glyph);
    REQUIRE(light.disabledFg != dark.disabledFg);
    REQUIRE(light.mutedStrong != dark.mutedStrong);

    REQUIRE(qGray(light.background) > qGray(dark.background));
    REQUIRE(qGray(light.surface) > qGray(dark.surface));
    REQUIRE(qGray(light.onSurface) < qGray(dark.onSurface));
    // The brand glyph darkens on light so it survives a white card: the shipped
    // SVG hex computes to 1.7:1 there.
    REQUIRE(qGray(light.glyph) < qGray(dark.glyph));
    // disabledFg is deliberately NOT in the "light is darker" set: a dead control
    // recedes, which on a white card means paler. Both clear 3:1 on their own
    // surface (test_theme_contrast.cpp).
}

TEST_CASE("paletteFor maps the appearance to its palette", "[settings][theme]") {
    REQUIRE(paletteFor(Appearance::Dark).background == darkPalette().background);
    REQUIRE(paletteFor(Appearance::Light).background == lightPalette().background);
}

namespace {

struct ThemeHarness {
    Observable<ThemeMode> mode{ThemeMode::System};
    Appearance systemAppearance = Appearance::Dark; // what SYSTEM resolves to
    std::vector<Appearance> applied;

    ThemeController controller{mode, [this] { return systemAppearance; },
                               [this](Appearance a) { applied.push_back(a); }};
};

} // namespace

TEST_CASE("ThemeController resolves SYSTEM via the stubbed OS reader (both ways)",
          "[settings][theme]") {
    SECTION("AppsUseLightTheme = 0 -> Dark") {
        ThemeHarness h;
        h.systemAppearance = Appearance::Dark;
        REQUIRE(h.controller.resolve(ThemeMode::System) == Appearance::Dark);
    }
    SECTION("AppsUseLightTheme = 1 -> Light") {
        ThemeHarness h;
        h.systemAppearance = Appearance::Light;
        REQUIRE(h.controller.resolve(ThemeMode::System) == Appearance::Light);
    }
    ThemeHarness h;
    h.systemAppearance = Appearance::Dark;
    REQUIRE(h.controller.resolve(ThemeMode::Light) == Appearance::Light);
    REQUIRE(h.controller.resolve(ThemeMode::Dark) == Appearance::Dark);
}

TEST_CASE("ThemeController re-themes on each mode change (store->controller->palette)",
          "[settings][theme]") {
    ThemeHarness h;
    h.systemAppearance = Appearance::Light; // SYSTEM resolves to Light here

    ControllerProbe<ThemeController> probe(h.controller);
    probe.start();
    // start() applies the current value immediately.
    REQUIRE(h.applied.size() == 1);
    REQUIRE(h.applied.back() == Appearance::Light);

    h.mode.set(ThemeMode::Dark);
    REQUIRE(h.applied.size() == 2);
    REQUIRE(h.applied.back() == Appearance::Dark);

    h.mode.set(ThemeMode::Light);
    REQUIRE(h.applied.size() == 3);
    REQUIRE(h.applied.back() == Appearance::Light);

    REQUIRE(h.applied ==
            std::vector<Appearance>{Appearance::Light, Appearance::Dark, Appearance::Light});
}

TEST_CASE("ThemeController: switching System resolution re-themes when re-emitted",
          "[settings][theme]") {
    ThemeHarness h;
    h.systemAppearance = Appearance::Dark;
    ControllerProbe<ThemeController> probe(h.controller);
    probe.start();
    REQUIRE(h.applied.back() == Appearance::Dark);
    h.mode.set(ThemeMode::Light);
    h.systemAppearance = Appearance::Light;
    h.mode.set(ThemeMode::System);
    REQUIRE(h.applied.back() == Appearance::Light);
}

// The QML ThemeBridge reads the dish::ui::Theme::* statics at render time, so
// applying a resolved appearance must leave those statics on that appearance's
// palette. The statics are process-global, hence the re-apply per section and
// the restore at the end.

namespace {

// Mirrors the production startup decision: resolve the mode (System through the
// OS reader) and apply it to the active palette.
void applyResolved(ThemeMode mode, Appearance systemReading) {
    const Appearance resolved = mode == ThemeMode::Light  ? Appearance::Light
                                : mode == ThemeMode::Dark ? Appearance::Dark
                                                          : systemReading;
    dish::ui::setActiveAppearance(resolved);
}

} // namespace

TEST_CASE("active Theme tokens match the resolved appearance after startup", "[settings][theme]") {
    SECTION("System + OS dark -> dark tokens (the reported repro)") {
        applyResolved(ThemeMode::System, Appearance::Dark);
        REQUIRE(dish::ui::activeAppearance() == Appearance::Dark);
        REQUIRE(dish::ui::Theme::surface == darkPalette().surface);
        REQUIRE(dish::ui::Theme::background == darkPalette().background);
        // The exact seam the QML Card binds (color: Theme.surface).
        REQUIRE(dish::ui::Theme::surface != lightPalette().surface);
    }
    SECTION("System + OS light -> light tokens") {
        applyResolved(ThemeMode::System, Appearance::Light);
        REQUIRE(dish::ui::activeAppearance() == Appearance::Light);
        REQUIRE(dish::ui::Theme::surface == lightPalette().surface);
        REQUIRE(dish::ui::Theme::background == lightPalette().background);
    }
    SECTION("explicit Light ignores an OS-dark reading -> light tokens") {
        applyResolved(ThemeMode::Light, Appearance::Dark);
        REQUIRE(dish::ui::activeAppearance() == Appearance::Light);
        REQUIRE(dish::ui::Theme::surface == lightPalette().surface);
    }
    SECTION("explicit Dark ignores an OS-light reading -> dark tokens") {
        applyResolved(ThemeMode::Dark, Appearance::Light);
        REQUIRE(dish::ui::activeAppearance() == Appearance::Dark);
        REQUIRE(dish::ui::Theme::surface == darkPalette().surface);
    }
    // Restore the process-global default for whatever test runs next.
    dish::ui::setActiveAppearance(Appearance::Dark);
}
