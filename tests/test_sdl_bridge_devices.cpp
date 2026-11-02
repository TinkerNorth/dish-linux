// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The SDL thread hands the GUI a new device list only when the list changed: a pad attaching, a
// pad leaving, or a pad's charge moving. Battery sweeps that read an unchanged charge hand over
// nothing, so they no longer wake the GUI's rebuild.

#include "HotPathRig.h"

#include "Input/GamepadInputProcessor.h"
#include "Input/SDLGamepadBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <QList>
#include <QObject>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace hp = dish::test::hotpath;
using Device = dish::input::SDLGamepadBridge::Device;

// What can move in a listed pad once it has attached: whether it is listed, and its charge.
using Entry = std::tuple<QString, std::uint8_t, std::uint8_t>;

// Runs the Qt loop, which delivers the bridge's queued signals, for `ms`.
void pumpFor(int ms) {
    hp::pumpUntil([] { return false; }, ms);
}

int listed(const QList<Device>& devices, const QString& id) {
    return static_cast<int>(std::count_if(devices.begin(), devices.end(),
                                          [&id](const Device& d) { return d.id == id; }));
}

// The lists the GUI is handed, as one pad sees them: those that moved its own entry, and those
// that moved nothing at all. A list that moved only another pad on the machine is neither.
class Handovers {
  public:
    explicit Handovers(QString padId) : padId_(std::move(padId)) {}

    void take(const QList<Device>& devices) {
        std::vector<Entry> all;
        std::optional<Entry> own;
        for (const Device& d : devices) {
            all.emplace_back(d.id, d.batteryLevel, d.batteryStatus);
            if (d.id == padId_) { own = all.back(); }
        }
        if (own != own_) { ++ownMoves_; }
        if (all == all_) { ++unchanged_; }
        own_ = std::move(own);
        all_ = std::move(all);
    }

    int ownMoves() const { return ownMoves_; }
    int unchanged() const { return unchanged_; }

  private:
    const QString padId_;
    std::optional<Entry> own_;
    std::vector<Entry> all_;
    int ownMoves_ = 0;
    int unchanged_ = 0;
};

} // namespace

TEST_CASE("an idle pad's battery sweeps hand the GUI no new device list", "[sdl-bridge][devices]") {
    dish::input::GamepadInputProcessor processor;
    dish::input::SDLGamepadBridge bridge(&processor);
    SDL_GetTicks64();
    bridge.start();
    REQUIRE(hp::pumpUntil([] { return SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0; }));
    std::optional<hp::VirtualPad> pad;
    pad.emplace("Battery sweep pad");
    REQUIRE(pad->ok());
    const QString padId = pad->deviceId();
    Handovers handovers(padId);
    QObject::connect(&bridge, &dish::input::SDLGamepadBridge::devicesChanged,
                     [&] { handovers.take(bridge.devices()); });
    REQUIRE(hp::pumpUntil([&] { return listed(bridge.devices(), padId) == 1; }));
    // The attach, then at most the first charge reading, which the next sweep takes.
    pumpFor(2500);
    const int settledMoves = handovers.ownMoves();
    const int settledUnchanged = handovers.unchanged();
    CHECK(settledMoves >= 1);
    CHECK(settledMoves <= 2);
    // Three more sweeps of an unchanged charge.
    pumpFor(3200);
    CHECK(handovers.ownMoves() == settledMoves);
    CHECK(handovers.unchanged() == settledUnchanged);
    CHECK(listed(bridge.devices(), padId) == 1);
    pad.reset();
    REQUIRE(hp::pumpUntil([&] { return listed(bridge.devices(), padId) == 0; }));
    bridge.stop();
}
