// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Driven through a fake tray item and a fake notifier over a temp INI, so no
// StatusNotifier host is registered, no desktop notification is raised and the
// user's real settings are never touched.

#include "composer/BackgroundCoordinator.h"
#include "core/reducer/BackgroundMode.h"
#include "source/notification/DesktopNotifier.h"
#include "source/store/BackgroundPreferenceStore.h"
#include "source/tray/TrayIcon.h"

#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QObject>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <vector>

using dish::composer::BackgroundCoordinator;
using dish::reducer::TrayPresentation;
using dish::reducer::WindowCloseAction;
using dish::source::BackgroundPreferenceStore;
using dish::test::StateSourceProbe;

namespace {

// Counts every call, so "registered once" and "announced once" are observable
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

    // The three things a real tray host can ask for.
    void emitShowWindowRequested() { emit showWindowRequested(); }
    void emitQuitRequested() { emit quitRequested(); }
    void emitAvailabilityChanged(bool available) { emit availabilityChanged(available); }

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

// Records rather than raises: the body is composed through translate(), so what
// is pinned here is that it fired and carried text, not the English wording.
class FakeNotifier : public dish::source::DesktopNotifier {
  public:
    struct Notice {
        QString summary;
        QString body;
    };

    void notify(const QString& summary, const QString& body) override {
        notices_.push_back(Notice{summary, body});
    }

    const std::vector<Notice>& notices() const { return notices_; }
    std::size_t count() const { return notices_.size(); }

  private:
    std::vector<Notice> notices_;
};

// QSignalSpy stand-in: DishTests links Catch2, not Qt6::Test.
struct CommandSpy {
    int showWindows = 0;
    int quits = 0;
    int availabilities = 0;
    bool lastAvailable = false;

    explicit CommandSpy(BackgroundCoordinator* coordinator) {
        QObject::connect(coordinator, &BackgroundCoordinator::showWindowRequested,
                         [this]() { ++showWindows; });
        QObject::connect(coordinator, &BackgroundCoordinator::quitRequested, [this]() { ++quits; });
        QObject::connect(coordinator, &BackgroundCoordinator::trayAvailabilityChanged,
                         [this](bool available) {
                             ++availabilities;
                             lastAvailable = available;
                         });
    }
};

std::unique_ptr<BackgroundPreferenceStore> makeStore(const QString& iniPath) {
    return std::make_unique<BackgroundPreferenceStore>(
        std::make_unique<QSettings>(iniPath, QSettings::IniFormat));
}

} // namespace

TEST_CASE("BackgroundCoordinator: a close hides when enabled and a tray host is there",
          "[background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);

    REQUIRE(store->runInBackground()); // the default
    REQUIRE(c.trayAvailable());
    REQUIRE(c.closeRequested() == WindowCloseAction::HideToBackground);
}

TEST_CASE("BackgroundCoordinator: a close quits when the tray host is missing", "[background]") {
    // The safety property, end to end: enabled alone never hides the window.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    tray.setAvailable(false);
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);

    REQUIRE(store->runInBackground());
    REQUIRE_FALSE(c.trayAvailable());
    REQUIRE(c.closeRequested() == WindowCloseAction::Quit);
}

TEST_CASE("BackgroundCoordinator: a close quits when the preference is off", "[background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    store->setRunInBackground(false);
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);

    REQUIRE(c.trayAvailable());
    REQUIRE(c.closeRequested() == WindowCloseAction::Quit);
}

TEST_CASE("BackgroundCoordinator: the notice fires once across repeated closes", "[background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);

    REQUIRE(c.closeRequested() == WindowCloseAction::HideToBackground);
    REQUIRE(notifier.count() == 1);
    REQUIRE_FALSE(notifier.notices().at(0).summary.isEmpty());
    REQUIRE_FALSE(notifier.notices().at(0).body.isEmpty());

    c.closeRequested();
    c.closeRequested();
    REQUIRE(notifier.count() == 1);
}

TEST_CASE("BackgroundCoordinator: the notice is spent on the store, not in memory",
          "[background]") {
    // Answering the question is what spends the notice, and the flag is
    // persisted, so a restart does not announce it all over again.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    FakeNotifier first;
    {
        BackgroundCoordinator c(store.get(), &tray, &first);
        REQUIRE(c.closeRequested() == WindowCloseAction::HideToBackground);
    }
    REQUIRE(first.count() == 1);
    REQUIRE(store->noticeShown());

    FakeNotifier second;
    BackgroundCoordinator restarted(store.get(), &tray, &second);
    REQUIRE(restarted.closeRequested() == WindowCloseAction::HideToBackground);
    REQUIRE(second.count() == 0);
}

TEST_CASE("BackgroundCoordinator: a quitting close announces nothing", "[background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    tray.setAvailable(false);
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);

    REQUIRE(c.closeRequested() == WindowCloseAction::Quit);
    REQUIRE(notifier.count() == 0);
    // The notice is unspent: it belongs to the first hide, whenever that comes.
    REQUIRE_FALSE(store->noticeShown());
}

TEST_CASE("BackgroundCoordinator: window visibility is published", "[background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);
    StateSourceProbe<bool> probe(c.windowVisible());
    // The one emission is the current value replayed to the new subscriber.
    REQUIRE(probe.count() == 1);
    REQUIRE(c.windowVisible().value());

    c.setWindowVisible(false);
    REQUIRE_FALSE(c.windowVisible().value());
    REQUIRE(probe.count() == 2);
    REQUIRE_FALSE(probe.states().back()); // vector<bool> yields a value, not a reference

    c.setWindowVisible(true);
    REQUIRE(probe.count() == 3);
}

TEST_CASE("BackgroundCoordinator: a repeat visibility set does not re-emit", "[background]") {
    // The tray presentation is derived from this, so a re-emit would redraw the
    // panel item on every window event that changed nothing.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);
    StateSourceProbe<bool> probe(c.windowVisible());

    c.setWindowVisible(true); // already visible
    REQUIRE(probe.count() == 1);

    c.setWindowVisible(false);
    REQUIRE(probe.count() == 2);
    c.setWindowVisible(false);
    REQUIRE(probe.count() == 2);
}

TEST_CASE("BackgroundCoordinator: the tray's commands are re-emitted", "[background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);
    CommandSpy spy(&c);

    tray.emitShowWindowRequested();
    REQUIRE(spy.showWindows == 1);

    tray.emitQuitRequested();
    REQUIRE(spy.quits == 1);

    // A host that goes away has to reach the close policy, or the next close
    // hides the window with nothing left to bring it back.
    tray.emitAvailabilityChanged(false);
    REQUIRE(spy.availabilities == 1);
    REQUIRE_FALSE(spy.lastAvailable);
    tray.emitAvailabilityChanged(true);
    REQUIRE(spy.availabilities == 2);
    REQUIRE(spy.lastAvailable);
}

TEST_CASE("BackgroundCoordinator: a tray host lost while hidden asks for the window back",
          "[background]") {
    // The rescue. A panel that crashes while the window is hidden leaves a
    // running Dish with nothing to click, so the window comes back by itself
    // rather than waiting for a click that can no longer happen.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);
    c.setWindowVisible(false);
    CommandSpy spy(&c);

    tray.emitAvailabilityChanged(false);
    REQUIRE(spy.showWindows == 1);
    REQUIRE(spy.availabilities == 1);
    REQUIRE_FALSE(spy.lastAvailable);
}

TEST_CASE("BackgroundCoordinator: a tray host lost while visible asks for nothing",
          "[background]") {
    // There is already a window on screen; raising it again would steal focus
    // for an event the user has no reason to care about.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);
    REQUIRE(c.windowVisible().value()); // visible by default
    CommandSpy spy(&c);

    tray.emitAvailabilityChanged(false);
    REQUIRE(spy.availabilities == 1);
    REQUIRE_FALSE(spy.lastAvailable);
    REQUIRE(spy.showWindows == 0);
}

TEST_CASE("BackgroundCoordinator: a tray host arriving asks for nothing", "[background]") {
    // Only the losing edge rescues. A host appearing is the case the whole
    // background mode exists for, whichever way the window happens to be.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), &tray, &notifier);
    CommandSpy spy(&c);

    tray.emitAvailabilityChanged(true);
    REQUIRE(spy.availabilities == 1);
    REQUIRE(spy.lastAvailable);
    REQUIRE(spy.showWindows == 0);

    c.setWindowVisible(false);
    tray.emitAvailabilityChanged(true);
    REQUIRE(spy.availabilities == 2);
    REQUIRE(spy.lastAvailable);
    REQUIRE(spy.showWindows == 0);
}

TEST_CASE("BackgroundCoordinator: tolerates a null tray item", "[background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeNotifier notifier;
    BackgroundCoordinator c(store.get(), nullptr, &notifier);

    REQUIRE_FALSE(c.trayAvailable());
    REQUIRE(c.closeRequested() == WindowCloseAction::Quit);
    REQUIRE(notifier.count() == 0);
    c.setWindowVisible(false);
    REQUIRE_FALSE(c.windowVisible().value());
}

TEST_CASE("BackgroundCoordinator: tolerates a null notifier", "[background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    FakeTrayIcon tray;
    BackgroundCoordinator c(store.get(), &tray, nullptr);

    REQUIRE(c.closeRequested() == WindowCloseAction::HideToBackground);
    // The notice is still spent: a missing notification service must not make
    // the coordinator retry the announcement on every close.
    REQUIRE(store->noticeShown());
}

TEST_CASE("BackgroundCoordinator: tolerates a null store", "[background]") {
    // With nowhere to read the preference from, closing has to mean quitting.
    FakeTrayIcon tray;
    FakeNotifier notifier;
    BackgroundCoordinator c(nullptr, &tray, &notifier);

    REQUIRE(c.trayAvailable());
    REQUIRE(c.closeRequested() == WindowCloseAction::Quit);
    REQUIRE(notifier.count() == 0);
}
