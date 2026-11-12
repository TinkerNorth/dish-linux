// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What a claimed pad's output worker owes it on a clock, beside the reports a
// host asks for. The 2026 Steam Controller's firmware turns lizard mode back on
// unless its settings keep arriving, so SDL re-sends them every 3 s while the
// pad is open; its haptics stop on their own about 50 ms after a rumble report,
// so SDL re-sends a running rumble every 40 ms (SDL_hidapi_steam_triton.c).
// Every other family owes nothing. Time is passed in, so the schedule tests
// against fixed instants; the worker's own thread is the only caller.

#pragma once

#include "core/input/UsbReportParsers.h"

#include <algorithm>
#include <chrono>
#include <optional>

namespace dish::input {

class OutputCadence {
  public:
    using Clock = std::chrono::steady_clock;

    // A family's periods; none for a family that owes nothing of that kind.
    struct Periods {
        std::optional<std::chrono::milliseconds> settings;
        std::optional<std::chrono::milliseconds> rumble;
    };

    static Periods periodsFor(usbparse::HidParser parser) {
        switch (parser) {
        case usbparse::HidParser::SteamTriton:
            return Periods{kTritonSettingsPeriod, kTritonRumblePeriod};
        case usbparse::HidParser::DualShock4:
        case usbparse::HidParser::DualSense:
        case usbparse::HidParser::SwitchProUsb:
        case usbparse::HidParser::GenericHid:
        case usbparse::HidParser::SteamController:
        case usbparse::HidParser::None:
            return Periods{};
        }
        return Periods{};
    }

    // The settings are owed at once: a claim sends none of its own.
    OutputCadence(usbparse::HidParser parser, Clock::time_point claimedAt)
        : periods_(periodsFor(parser)) {
        if (periods_.settings.has_value()) { settingsDueAt_ = claimedAt; }
    }

    bool settingsDue(Clock::time_point now) const {
        return settingsDueAt_.has_value() && now >= *settingsDueAt_;
    }

    // Whether the family's settings ride this clock at all, so a pad that is there holds them
    // without a write of the worker's own.
    bool sendsSettings() const { return periods_.settings.has_value(); }

    // Due again a period after this send, whether or not the pad took it: a write
    // that missed is the next period's to repeat, as SDL's is.
    void settingsSent(Clock::time_point at) {
        if (periods_.settings.has_value()) { settingsDueAt_ = at + *periods_.settings; }
    }

    bool rumbleDue(Clock::time_point now) const {
        return rumbleDueAt_.has_value() && now >= *rumbleDueAt_;
    }

    // `running` is whether either motor was asked to move. A running rumble is
    // owed again a period after every write of it; a stop is written once.
    void rumbleSent(Clock::time_point at, bool running) {
        if (!running || !periods_.rumble.has_value()) {
            rumbleDueAt_.reset();
            return;
        }
        rumbleDueAt_ = at + *periods_.rumble;
    }

    // The earliest instant anything is owed, or none when nothing ever is.
    std::optional<Clock::time_point> nextDue() const {
        if (!settingsDueAt_.has_value()) { return rumbleDueAt_; }
        if (!rumbleDueAt_.has_value()) { return settingsDueAt_; }
        return std::min(*settingsDueAt_, *rumbleDueAt_);
    }

  private:
    static constexpr std::chrono::milliseconds kTritonSettingsPeriod{3000};
    static constexpr std::chrono::milliseconds kTritonRumblePeriod{40};

    const Periods periods_;
    std::optional<Clock::time_point> settingsDueAt_;
    std::optional<Clock::time_point> rumbleDueAt_;
};

} // namespace dish::input
