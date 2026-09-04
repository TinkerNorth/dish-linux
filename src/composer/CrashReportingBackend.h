// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The seam a crash-reporting opt-in flip is routed to.
//
// Two separate things happen when Dish crashes, and only one of them is behind
// this switch:
//
//   * UI/CrashHandler always writes a backtrace to
//     $XDG_STATE_HOME/dish/crash.log, and UI/CrashReport turns it into the
//     redacted report the user reads and can send themselves. That is local,
//     private, and armed first thing in main() before the preference has even
//     been read, because a crash during startup still deserves an artifact.
//     Nothing here can turn it off.
//   * Uploading a report to Sentry. That is what this backend does, and it is
//     what the Settings switch means.
//
// The switch is opt-OUT (CrashReportingStore::kDefaultEnabled is true), matching
// dish-android and dish-windows.
//
// A build additionally has to carry a DSN before any of this can transmit.
// DISH_SENTRY_DSN is empty in CMake and only release.yml fills it in, from a
// repository secret, so a local build, a PR build and a build from a fork
// cannot report at all. On this platform the DSN decides more than that: because
// sentry-native has no Debian or Ubuntu package, the SDK is only fetched and
// built when a DSN is present, so a build without one does not pay for it.
// That also makes the Sentry environment safe to derive from the DSN rather than
// track separately: a build with one is "production", without is "development",
// and the two can never disagree.

#pragma once

#include <string>

namespace dish::composer {

class CrashReportingBackend {
  public:
    virtual ~CrashReportingBackend() = default;
    virtual void setEnabled(bool enabled) = 0;
};

// Arms and disarms the Sentry native SDK. Inert when the SDK was not compiled
// in, which is every build without a DSN, so an ordinary source checkout still
// builds and behaves with no extra dependency.
class SentryCrashReportingBackend : public CrashReportingBackend {
  public:
    // Where the SDK keeps its run state and any pending envelope. Empty (the
    // default) resolves to QStandardPaths::AppLocalDataLocation + "/sentry" at
    // arm time, which is a real per-user writable location rather than a working
    // directory Dish does not control.
    explicit SentryCrashReportingBackend(std::string databaseDir = {});
    ~SentryCrashReportingBackend() override;

    // Disarming is immediate and deliberate: withdrawing consent has to stop
    // the next crash from being sent, not the one after that.
    void setEnabled(bool enabled) override;

    // True only once the SDK actually armed. False on a build with no DSN even
    // when the switch is on, which is what lets the UI avoid claiming reports
    // are being sent when they cannot be.
    bool active() const { return active_; }

  private:
    // Closes the SDK if it is armed. noexcept and free of logging or
    // allocation, so the destructor can call it: a destructor that re-entered
    // setEnabled() would inherit every throwing path in it.
    void disarm() noexcept;

    // maybe_unused because its only read is inside the SDK guard: a stub
    // build (no DSN, so no SDK fetched) would otherwise trip clang's
    // -Wunused-private-field, which -Wall turns on and -Werror makes fatal.
    [[maybe_unused]] std::string databaseDir_;
    bool active_ = false;
};

// True when this build was linked against the Sentry SDK at all.
bool sentrySdkAvailable();

// The DSN compiled into this build, empty for every build release.yml did not
// produce. Exposed for the tests and for the honesty check above.
const char* compiledSentryDsn();

// "production" when a DSN is compiled in, "development" otherwise. Derived from
// the DSN rather than tracked separately so the label cannot drift from the
// thing that actually decides whether anything is sent.
const char* sentryEnvironment();

// Whether a build carrying this DSN, with the operator's switch in this
// position, should arm. Pure, so the policy is testable without the SDK.
bool shouldArmSentry(const char* compiledDsn, const char* envOverride, bool userEnabled);

} // namespace dish::composer
