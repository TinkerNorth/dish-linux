// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The derive half of the tray. Both upstreams are plain Observables here, so
// the composer is exercised without a panel, a session bus or a live slot.

#include "architecture/Observable.h"
#include "composer/TrayComposer.h"

#include "ComposerProbe.h"

#include <catch2/catch_test_macros.hpp>

using dish::arch::Observable;
using dish::composer::TrayComposer;
using dish::reducer::TrayActivity;
using dish::reducer::TrayPresentation;
using dish::test::ComposerProbe;

TEST_CASE("TrayComposer: eager initial reflects the current inputs", "[tray]") {
    // The controller applies the current value the moment it starts, so an
    // initial that lagged behind would register the item showing the wrong
    // thing until the first upstream change.
    Observable<bool> windowVisible(false);
    Observable<int> slotCount(2);
    TrayComposer composer(windowVisible, slotCount);

    const auto initial = composer.state().value();
    REQUIRE(initial.activity == TrayActivity::Streaming);
    REQUIRE(initial.streamingSlots == 2);
    REQUIRE_FALSE(initial.windowVisible);
}

TEST_CASE("TrayComposer: a window-visibility change recomputes", "[tray]") {
    Observable<bool> windowVisible(true);
    Observable<int> slotCount(0);
    TrayComposer composer(windowVisible, slotCount);
    ComposerProbe<TrayPresentation> probe(composer.state());

    windowVisible.set(false);
    REQUIRE_FALSE(composer.state().value().windowVisible);
    // eager initial + the change
    REQUIRE(probe.count() == 2);
}

TEST_CASE("TrayComposer: a streaming-count change recomputes", "[tray]") {
    Observable<bool> windowVisible(true);
    Observable<int> slotCount(0);
    TrayComposer composer(windowVisible, slotCount);
    ComposerProbe<TrayPresentation> probe(composer.state());

    slotCount.set(1);
    REQUIRE(composer.state().value().activity == TrayActivity::Streaming);
    REQUIRE(composer.state().value().streamingSlots == 1);

    slotCount.set(0);
    REQUIRE(composer.state().value().activity == TrayActivity::Idle);
    REQUIRE(probe.count() == 3); // initial(0) + ->1 + ->0
}

TEST_CASE("TrayComposer: distinct-until-changed suppresses a same-value push", "[tray]") {
    Observable<bool> windowVisible(true);
    Observable<int> slotCount(1);
    TrayComposer composer(windowVisible, slotCount);
    ComposerProbe<TrayPresentation> probe(composer.state());
    REQUIRE(probe.count() == 1);

    slotCount.set(1);
    REQUIRE(probe.count() == 1);
    windowVisible.set(true);
    REQUIRE(probe.count() == 1);

    // A real change still lands.
    slotCount.set(2);
    REQUIRE(probe.count() == 2);
}

TEST_CASE("TrayComposer: an upstream change that derives the same value does not re-emit",
          "[tray]") {
    // The clamp collapses every negative count onto zero, so the upstream moves
    // while the presentation does not: the guard is on the derived value, not
    // on the inputs, and the panel is not asked to redraw the same item.
    Observable<bool> windowVisible(true);
    Observable<int> slotCount(0);
    TrayComposer composer(windowVisible, slotCount);
    ComposerProbe<TrayPresentation> probe(composer.state());

    slotCount.set(-1);
    slotCount.set(-2);
    REQUIRE(probe.count() == 1);
    REQUIRE(composer.state().value().activity == TrayActivity::Idle);
}
