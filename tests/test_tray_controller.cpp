// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Driven through a fake tray item, so no StatusNotifier host is registered for
// the test process and nothing is left on a real panel.

#include "architecture/Observable.h"
#include "composer/TrayComposer.h"
#include "composer/TrayController.h"
#include "source/tray/TrayIcon.h"

#include "ControllerProbe.h"

#include <catch2/catch_test_macros.hpp>

using dish::arch::Observable;
using dish::composer::TrayComposer;
using dish::composer::TrayController;
using dish::reducer::TrayActivity;
using dish::reducer::TrayPresentation;
using dish::test::ControllerProbe;

namespace {

// Counts every call, so "registered once" and "redrawn once" are observable
// rather than inferred from the last value alone.
class FakeTrayIcon : public dish::source::TrayIcon {
  public:
    void show() override { ++shows_; }
    void hide() override { ++hides_; }
    bool isAvailable() const override { return available_; }
    void setPresentation(const TrayPresentation& presentation) override {
        ++presentations_;
        last_ = presentation;
    }

    void setAvailable(bool available) { available_ = available; }

    int shows() const { return shows_; }
    int hides() const { return hides_; }
    int presentations() const { return presentations_; }
    TrayPresentation last() const { return last_; }

  private:
    int shows_ = 0;
    int hides_ = 0;
    int presentations_ = 0;
    bool available_ = true;
    TrayPresentation last_;
};

TrayPresentation streaming(int slotCount) {
    return TrayPresentation{TrayActivity::Streaming, slotCount, true};
}
TrayPresentation idle() { return TrayPresentation{TrayActivity::Idle, 0, true}; }

} // namespace

TEST_CASE("TrayController: start registers the item and applies at once", "[tray]") {
    Observable<TrayPresentation> presentation(streaming(1));
    FakeTrayIcon fake;
    TrayController c(presentation, &fake);
    REQUIRE(fake.shows() == 0); // not started yet
    c.start();
    REQUIRE(fake.shows() == 1);
    REQUIRE(fake.presentations() == 1);
    REQUIRE(fake.last() == streaming(1));
}

TEST_CASE("TrayController: a second start does not re-register", "[tray]") {
    Observable<TrayPresentation> presentation(idle());
    FakeTrayIcon fake;
    TrayController c(presentation, &fake);
    c.start();
    c.start();
    REQUIRE(fake.shows() == 1);
    REQUIRE(fake.presentations() == 1);
}

TEST_CASE("TrayController: a presentation change is forwarded", "[tray]") {
    Observable<TrayPresentation> presentation(idle());
    FakeTrayIcon fake;
    TrayController c(presentation, &fake);
    c.start();
    presentation.set(streaming(2));
    REQUIRE(fake.presentations() == 2);
    REQUIRE(fake.last() == streaming(2));
    REQUIRE(fake.last().streamingSlots == 2);
}

TEST_CASE("TrayController: stop unregisters the item", "[tray]") {
    Observable<TrayPresentation> presentation(streaming(1));
    FakeTrayIcon fake;
    TrayController c(presentation, &fake);
    c.start();
    c.stop();
    // A quit that left the item behind would leave a dead entry on the panel.
    REQUIRE(fake.hides() == 1);
}

TEST_CASE("TrayController: changes after stop do not reach the item", "[tray]") {
    Observable<TrayPresentation> presentation(idle());
    FakeTrayIcon fake;
    TrayController c(presentation, &fake);
    c.start();
    c.stop();
    const int applied = fake.presentations();
    presentation.set(streaming(3));
    REQUIRE(fake.presentations() == applied);
}

TEST_CASE("TrayController: start after stop re-arms and re-applies", "[tray]") {
    Observable<TrayPresentation> presentation(streaming(1));
    FakeTrayIcon fake;
    TrayController c(presentation, &fake);
    c.start();
    c.stop();
    c.start(); // onStarting re-registers and re-applies the current value
    REQUIRE(fake.shows() == 2);
    REQUIRE(fake.hides() == 1);
    REQUIRE(fake.presentations() == 2);
    REQUIRE(fake.last() == streaming(1));
}

TEST_CASE("TrayController: after restart, changes actuate again", "[tray]") {
    Observable<TrayPresentation> presentation(idle());
    FakeTrayIcon fake;
    TrayController c(presentation, &fake);
    c.start();
    c.stop();
    c.start();
    presentation.set(streaming(4));
    REQUIRE(fake.last() == streaming(4));
}

TEST_CASE("TrayController: ControllerProbe start/stop drives the effect", "[tray]") {
    Observable<TrayPresentation> presentation(streaming(1));
    FakeTrayIcon fake;
    TrayController c(presentation, &fake);
    ControllerProbe<TrayController> probe(c);
    probe.start();
    REQUIRE(fake.shows() == 1);
    probe.stop();
    REQUIRE(fake.hides() == 1);
}

TEST_CASE("TrayController: tolerates a null tray item", "[tray]") {
    // A headless build has no panel to register with; the controller still
    // bookkeeps so start/stop stay symmetric.
    Observable<TrayPresentation> presentation(idle());
    TrayController c(presentation, nullptr);
    c.start();
    presentation.set(streaming(1));
    presentation.set(idle());
    c.stop();
    c.start();
    c.stop();
}

TEST_CASE("TrayController: end-to-end via the composer follows the slot count", "[tray]") {
    Observable<bool> windowVisible(true);
    Observable<int> slotCount(0);
    TrayComposer composer(windowVisible, slotCount);
    FakeTrayIcon fake;
    TrayController c(composer.state(), &fake);
    c.start();
    REQUIRE(fake.last().activity == TrayActivity::Idle);

    slotCount.set(2);
    REQUIRE(fake.last().activity == TrayActivity::Streaming);
    REQUIRE(fake.last().streamingSlots == 2);

    windowVisible.set(false);
    REQUIRE_FALSE(fake.last().windowVisible);

    slotCount.set(0);
    REQUIRE(fake.last().activity == TrayActivity::Idle);
}
