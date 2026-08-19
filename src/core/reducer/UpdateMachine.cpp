// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One reducer arm per event, with the phase guards inside it: the checker's
// interesting rules are about WHAT arrived, not where it arrived. The tests pin
// the effect lists in order, so a reordering is a behaviour change, not a
// cleanup.

#include "core/reducer/UpdateMachine.h"

#include "core/update/UpdateVersion.h"

namespace dish::reducer {

namespace {

template <class T> const T* as(const UpdateEvent& e) { return std::get_if<T>(&e); }

UpdateReduction stay(UpdateStatus s) { return UpdateReduction{std::move(s), {}}; }

UpdateReduction settleFailure(const UpdateStatus& s, UpdateError error) {
    UpdateStatus n = s;
    n.consecutiveFailures = s.consecutiveFailures + 1;
    n.phase = UpdatePhase::Failed;
    n.error = error;
    return UpdateReduction{
        std::move(n), {update_effect::ScheduleNextCheck{backoffDelayMs(n.consecutiveFailures)}}};
}

} // namespace

UpdateReduction reduceUpdate(const UpdateStatus& s, const UpdateEvent& event) {
    if (const auto* prefs = as<update_event::PrefsChanged>(event)) {
        UpdateStatus n = s;
        n.checksEnabled = prefs->checksEnabled;
        n.skippedVersion = prefs->skippedVersion;

        if (!prefs->checksEnabled) {
            // Off means off from EVERY phase: no timer is re-armed and the
            // checker constructs no QNAM while Disabled.
            n.phase = UpdatePhase::Disabled;
            n.error = UpdateError::None;
            return stay(std::move(n));
        }
        if (s.phase == UpdatePhase::Disabled) {
            // Re-enabling behaves exactly like a cold start.
            n.phase = UpdatePhase::Idle;
            return UpdateReduction{std::move(n),
                                   {update_effect::ScheduleNextCheck{kStartupDelayMs}}};
        }
        return stay(std::move(n));
    }

    if (const auto* req = as<update_event::CheckRequested>(event)) {
        if (s.phase == UpdatePhase::Disabled || s.phase == UpdatePhase::Checking) {
            return stay(s);
        }
        if (!s.online) {
            // The reachability gate answers WITHOUT touching the network, which
            // is the whole point: a captive portal never gets a request.
            return settleFailure(s, UpdateError::Offline);
        }
        UpdateStatus n = s;
        n.phase = UpdatePhase::Checking;
        return UpdateReduction{
            std::move(n), {update_effect::FetchManifest{req->trigger == UpdateTrigger::Manual}}};
    }

    if (const auto* arrived = as<update_event::ManifestArrived>(event)) {
        if (s.phase != UpdatePhase::Checking) { return stay(s); }
        const dish::update::UpdateManifest& m = arrived->manifest;

        UpdateStatus n = s;
        n.minimumSupportedVersion = m.minimumSupportedVersion;
        n.required = dish::update::isStrictlyNewer(m.minimumSupportedVersion, s.currentVersion);
        n.consecutiveFailures = 0;
        n.error = UpdateError::None;

        std::vector<UpdateEffect> fx{update_effect::PersistLastCheck{}};

        const bool newer = dish::update::isStrictlyNewer(m.version, s.currentVersion);
        const bool skipped = !s.skippedVersion.isEmpty() && s.skippedVersion == m.version;

        if (!newer || (skipped && !n.required)) {
            // Nothing to offer, or the user muted this exact version. A skip is
            // ignored while the running build is below the supported minimum.
            n.phase = UpdatePhase::UpToDate;
            n.availableVersion.clear();
            n.notesUrl.clear();
            fx.push_back(update_effect::ScheduleNextCheck{kPeriodicIntervalMs});
            return UpdateReduction{std::move(n), std::move(fx)};
        }

        n.phase = UpdatePhase::Available;
        n.availableVersion = m.version;
        n.notesUrl = m.releaseNotesUrl;
        if (n.required) {
            fx.push_back(update_effect::Notify{UpdateNotice::Unsupported, m.version});
        }
        fx.push_back(update_effect::Notify{UpdateNotice::Available, m.version});
        fx.push_back(update_effect::ScheduleNextCheck{kPeriodicIntervalMs});
        return UpdateReduction{std::move(n), std::move(fx)};
    }

    if (const auto* failed = as<update_event::CheckFailed>(event)) {
        if (s.phase != UpdatePhase::Checking) { return stay(s); }
        return settleFailure(s, failed->error);
    }

    if (const auto* skip = as<update_event::SkipRequested>(event)) {
        // A required update cannot be skipped: the build is already unsupported.
        if (s.required || skip->version.isEmpty()) { return stay(s); }
        UpdateStatus n = s;
        n.skippedVersion = skip->version;
        if (s.availableVersion == skip->version) {
            n.phase = UpdatePhase::UpToDate;
            n.availableVersion.clear();
            n.notesUrl.clear();
        }
        return stay(std::move(n));
    }

    if (const auto* reach = as<update_event::ReachabilityChanged>(event)) {
        UpdateStatus n = s;
        n.online = reach->online;
        const bool wasOfflineGated =
            s.phase == UpdatePhase::Failed && s.error == UpdateError::Offline && s.checksEnabled;
        if (reach->online && wasOfflineGated) {
            return UpdateReduction{std::move(n),
                                   {update_effect::ScheduleNextCheck{kReconnectCheckDelayMs}}};
        }
        return stay(std::move(n));
    }

    return stay(s);
}

} // namespace dish::reducer
