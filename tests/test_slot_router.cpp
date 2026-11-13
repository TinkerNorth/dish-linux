// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The published route table, over the production objects the hot-path rig wires: a report goes
// where the table the GUI last published says, feedback lands where the slot's path and switches
// say, and a table is retired only once no thread can still be reading it.

#include "HotPathRig.h"

#include "FeatureSettings.h"
#include "core/model/Protocol.h"
#include "core/reducer/DirectPadKey.h"
#include "core/thread/TrackedMutex.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace hp = dish::test::hotpath;
using dish::input::GamepadInputProcessor;
using namespace std::chrono_literals;

constexpr int kQuietMs = 150;

// What the one session's satellite saw.
struct Seen {
    std::atomic<std::uint64_t> inputs{0};
    std::atomic<std::uint64_t> motions{0};
    std::atomic<int> controllerIndex{-1};
};

hp::HotPathRig::HandlerFor countInto(Seen& seen) {
    return [&seen](int /*session*/) {
        return [&seen](std::uint16_t type, const std::uint8_t* body, std::size_t len,
                       hp::Clock::time_point /*arrived*/) {
            if (type == dish::proto::kMsgInput && len > 0) {
                seen.controllerIndex.store(body[0]);
                seen.inputs.fetch_add(1);
            }
            if (type == dish::proto::kMsgMotion) { seen.motions.fetch_add(1); }
        };
    };
}

// A report that differs from the one before, so the latch never holds it back.
class Mover {
  public:
    Mover(GamepadInputProcessor& processor, GamepadInputProcessor::DeviceIndex index)
        : processor_(processor), index_(index) {}
    void next() {
        GamepadInputProcessor::DeviceState s;
        s.rx = static_cast<std::int16_t>(4096 + (step_++ % 1000) * 16);
        processor_.publish(index_, s);
    }

  private:
    GamepadInputProcessor& processor_;
    GamepadInputProcessor::DeviceIndex index_;
    int step_ = 0;
};

bool staysAt(const std::atomic<std::uint64_t>& counter, std::uint64_t value) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kQuietMs));
    return counter.load() == value;
}

void sendRumble(hp::LoopbackSatellite& satellite, std::uint16_t strong) {
    std::array<std::uint8_t, 7> rumble{0, 0, 0, 0x20, 0x00, 0x00, 0x64};
    hp::writeBe16(rumble.data() + 1, strong);
    satellite.sendToDish(dish::proto::kMsgRumble, rumble.data(), rumble.size());
}

} // namespace

TEST_CASE("a slot's reports reach its session only while the published table routes them",
          "[slot-router]") {
    Seen seen;
    hp::HotPathRig rig(1, countInto(seen));
    REQUIRE(rig.startBridge());
    REQUIRE(rig.connectAll());
    const QString slot = QStringLiteral("route-pad");
    REQUIRE(rig.bind(slot, 0));
    const auto index = rig.processor().add(slot.toStdString());
    REQUIRE(index.has_value());
    Mover pad(rig.processor(), *index);

    rig.router().republish({slot});
    pad.next();
    REQUIRE(hp::pumpUntil([&] { return seen.inputs.load() >= 1; }));
    CHECK(seen.controllerIndex.load() == 0);

    rig.router().republish({});
    const std::uint64_t routed = seen.inputs.load();
    pad.next();
    pad.next();
    CHECK(staysAt(seen.inputs, routed));

    rig.router().republish({slot});
    pad.next();
    CHECK(hp::pumpUntil([&] { return seen.inputs.load() > routed; }));
}

namespace {

// The INPUT frame as the one session's satellite received it: its length and
// the word behind the report, when the frame is long enough to carry one.
struct SeenInput {
    std::atomic<std::uint64_t> inputs{0};
    std::atomic<std::size_t> length{0};
    std::atomic<std::uint32_t> word{0};
};

constexpr std::size_t kInputWithWordBytes = 17;
constexpr std::size_t kInputWordOffset = 13;

std::uint32_t readWordLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

hp::HotPathRig::HandlerFor captureInput(SeenInput& seen) {
    return [&seen](int /*session*/) {
        return [&seen](std::uint16_t type, const std::uint8_t* body, std::size_t len,
                       hp::Clock::time_point /*arrived*/) {
            if (type != dish::proto::kMsgInput) { return; }
            seen.length.store(len);
            if (len >= kInputWithWordBytes) {
                seen.word.store(readWordLe32(body + kInputWordOffset));
            }
            seen.inputs.fetch_add(1);
        };
    };
}

} // namespace

TEST_CASE("a report on the rig's protocol 4 session is the 17-byte INPUT frame carrying the "
          "extended-buttons word",
          "[slot-router]") {
    SeenInput seen;
    hp::HotPathRig rig(1, captureInput(seen));
    REQUIRE(rig.startBridge());
    REQUIRE(rig.connectAll());
    const QString slot = QStringLiteral("paddle-pad");
    REQUIRE(rig.bind(slot, 0));
    const auto index = rig.processor().add(slot.toStdString());
    REQUIRE(index.has_value());
    rig.router().republish({slot});

    GamepadInputProcessor::DeviceState s;
    s.extButtons = dish::proto::kExtButtonLeftPaddle1 | dish::proto::kExtButtonMisc1;
    rig.processor().publish(*index, s);
    REQUIRE(hp::pumpUntil([&] { return seen.inputs.load() >= 1; }));
    CHECK(seen.length.load() == kInputWithWordBytes);
    CHECK(seen.word.load() == (dish::proto::kExtButtonLeftPaddle1 | dish::proto::kExtButtonMisc1));
}

namespace {

QString directSlot(int vendorId, int productId, int slot = dish::reducer::kNoReceiverSlot) {
    return QString::fromStdString(
        dish::reducer::directSlotId(dish::reducer::DirectPadKey{vendorId, productId, slot}));
}

} // namespace

TEST_CASE("a slot sources the extended buttons when its path's pad has them",
          "[slot-router][extended-buttons]") {
    Seen seen;
    hp::HotPathRig rig(1, countInto(seen));
    REQUIRE(rig.startBridge());
    // Direct: the model table answers. Both Steam Controllers (a wired pad and a
    // receiver's slot alike), the DualSense Edge and the Switch Pro source the
    // word; a DualShock 4 and a plain DualSense do not.
    CHECK(rig.router().slotHardware(directSlot(0x28DE, 0x1102)).hasExtendedButtons);
    CHECK(rig.router().slotHardware(directSlot(0x28DE, 0x1142, 2)).hasExtendedButtons);
    CHECK(rig.router().slotHardware(directSlot(0x28DE, 0x1302)).hasExtendedButtons);
    CHECK(rig.router().slotHardware(directSlot(0x054C, 0x0DF2)).hasExtendedButtons);
    CHECK(rig.router().slotHardware(directSlot(0x057E, 0x2009)).hasExtendedButtons);
    CHECK_FALSE(rig.router().slotHardware(directSlot(0x054C, 0x05C4)).hasExtendedButtons);
    CHECK_FALSE(rig.router().slotHardware(directSlot(0x054C, 0x0CE6)).hasExtendedButtons);
    // Standard: SDL's own mapping answers. A virtual game controller maps every
    // SDL button, the paddles and misc1 among them.
    const hp::VirtualPad pad("Paddle pad");
    REQUIRE(pad.ok());
    const QString padId = pad.deviceId();
    REQUIRE(hp::pumpUntil([&] { return rig.router().slotHardware(padId).hasRumble; }));
    CHECK(rig.router().slotHardware(padId).hasExtendedButtons);
    CHECK_FALSE(rig.router().slotHardware(QStringLiteral("sdl:999")).hasExtendedButtons);
}

TEST_CASE("a slot on screen but bound to no session sends nothing", "[slot-router]") {
    Seen seen;
    hp::HotPathRig rig(1, countInto(seen));
    REQUIRE(rig.startBridge());
    REQUIRE(rig.connectAll());
    const QString slot = QStringLiteral("lonely-pad");
    rig.router().republish({slot});
    const auto index = rig.processor().add(slot.toStdString());
    REQUIRE(index.has_value());
    Mover pad(rig.processor(), *index);
    pad.next();
    pad.next();
    CHECK(staysAt(seen.inputs, 0));
}

TEST_CASE("the motion switch, as published, gates the motion stream", "[slot-router]") {
    Seen seen;
    hp::HotPathRig rig(1, countInto(seen));
    REQUIRE(rig.startBridge());
    REQUIRE(rig.connectAll());
    const QString slot = QStringLiteral("gyro-pad");
    REQUIRE(rig.bind(slot, 0));
    const auto index = rig.processor().add(slot.toStdString());
    REQUIRE(index.has_value());
    const GamepadInputProcessor::MotionSample sample{10, 20, 30, 0, 8192, 0};
    std::uint64_t nowUs = 1'000'000;
    const auto tick = [&] {
        nowUs += GamepadInputProcessor::kMotionMinIntervalUs;
        rig.processor().publishMotionAt(*index, sample, nowUs);
    };

    rig.motionSwitch().setEnabled(slot.toStdString(), false);
    rig.router().republish({slot});
    tick();
    tick();
    CHECK(staysAt(seen.motions, 0));

    rig.motionSwitch().setEnabled(slot.toStdString(), true);
    rig.router().republish({slot});
    tick();
    CHECK(hp::pumpUntil([&] { return seen.motions.load() >= 1; }));
}

TEST_CASE("a session's rumble reaches its pad as the published switch allows", "[slot-router]") {
    Seen seen;
    hp::HotPathRig rig(1, countInto(seen));
    REQUIRE(rig.startBridge());
    const hp::VirtualPad pad("Router rumble pad");
    REQUIRE(pad.ok());
    const QString padId = pad.deviceId();
    REQUIRE(hp::pumpUntil([&] { return !rig.bridge().devices().isEmpty(); }));
    REQUIRE(rig.connectAll());
    REQUIRE(rig.bind(padId, 0));
    REQUIRE(hp::pumpUntil([&] { return rig.satellite(0).knowsDish(); }));

    rig.router().republish({padId});
    sendRumble(rig.satellite(0), 0x4000);
    CHECK(hp::pumpUntil([&] { return pad.lastRumble() == ((0x4000U << 16) | 0x2000U); }));

    rig.rumbleSwitch().setEnabled(padId, false);
    rig.router().republish({padId});
    sendRumble(rig.satellite(0), 0x5000);
    CHECK(hp::pumpUntil([&] { return pad.lastRumble() == 0U; }));
}

TEST_CASE("a session's light-bar colour lands only while it follows the game", "[slot-router]") {
    Seen seen;
    hp::HotPathRig rig(1, countInto(seen));
    REQUIRE(rig.startBridge());
    const hp::VirtualPad pad("Router light-bar pad");
    REQUIRE(pad.ok());
    const QString padId = pad.deviceId();
    REQUIRE(hp::pumpUntil([&] { return !rig.bridge().devices().isEmpty(); }));
    REQUIRE(rig.connectAll());
    REQUIRE(rig.bind(padId, 0));
    REQUIRE(hp::pumpUntil([&] { return rig.satellite(0).knowsDish(); }));
    rig.router().republish({padId});
    const std::array<std::uint8_t, 4> colour{0, 0x10, 0x20, 0x30};

    rig.features().setLightbarMode(dish::LightbarMode::Off);
    rig.satellite(0).sendToDish(dish::proto::kMsgLightbar, colour.data(), colour.size());
    rig.satellite(0).sendToDish(dish::proto::kMsgLightbar, colour.data(), colour.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(kQuietMs));
    CHECK(rig.router().lightbarsRouted() == 0);

    rig.features().setLightbarMode(dish::LightbarMode::FollowGame);
    rig.satellite(0).sendToDish(dish::proto::kMsgLightbar, colour.data(), colour.size());
    CHECK(hp::pumpUntil([&] { return rig.router().lightbarsRouted() >= 1; }));
}

TEST_CASE("republishing under a reporting thread never pulls a table from under it",
          "[slot-router]") {
    Seen seen;
    hp::HotPathRig rig(1, countInto(seen));
    REQUIRE(rig.startBridge());
    REQUIRE(rig.connectAll());
    const QString slot = QStringLiteral("busy-pad");
    REQUIRE(rig.bind(slot, 0));
    const auto index = rig.processor().add(slot.toStdString());
    REQUIRE(index.has_value());
    rig.router().republish({slot});

    std::atomic<bool> stop{false};
    std::thread reporter([&] {
        const dish::thread::ScopedThreadRole role(dish::thread::ThreadRole::SdlInput);
        Mover pad(rig.processor(), *index);
        while (!stop.load(std::memory_order_relaxed)) { pad.next(); }
    });
    for (int round = 0; round < 300; ++round) {
        rig.router().republish(round % 2 == 0 ? QStringList{} : QStringList{slot});
    }
    rig.router().republish({slot});
    stop.store(true);
    reporter.join();
    CHECK(hp::pumpUntil([&] { return seen.inputs.load() > 0; }));
}

namespace {

// The event time on the wire of the last touch frame the one session's satellite saw.
struct TouchStamps {
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint32_t> last{0};
};

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

hp::HotPathRig::HandlerFor stampsInto(TouchStamps& seen) {
    return [&seen](int /*session*/) {
        return [&seen](std::uint16_t type, const std::uint8_t* body, std::size_t len,
                       hp::Clock::time_point /*arrived*/) {
            // The stamp is u32 LE at 12 in the 16-byte v1 frame and at 13 in the 19-byte v2 one.
            if (type != dish::proto::kMsgTouchpad || (len != 16 && len != 19)) { return; }
            seen.last.store(readLe32(body + (len == 16 ? 12 : 13)));
            seen.frames.fetch_add(1);
        };
    };
}

std::uint32_t uptimeMs() {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

TEST_CASE("a touch frame keeps the stamp its source gave it, and an unstamped one is stamped now",
          "[slot-router]") {
    TouchStamps seen;
    hp::HotPathRig rig(1, stampsInto(seen));
    REQUIRE(rig.startBridge());
    REQUIRE(rig.connectAll());
    const QString slot = QStringLiteral("touch-pad");
    REQUIRE(rig.bind(slot, 0));
    const auto index = rig.processor().add(slot.toStdString());
    REQUIRE(index.has_value());
    rig.router().republish({slot});

    GamepadInputProcessor::TouchpadSample touch;
    touch.finger0Active = true;
    touch.eventTimeMs = 4242;
    rig.processor().publishTouchpad(*index, touch);
    REQUIRE(hp::pumpUntil([&] { return seen.frames.load() >= 1; }));
    CHECK(seen.last.load() == 4242);

    touch.eventTimeMs = 0;
    const std::uint32_t before = uptimeMs();
    rig.processor().publishTouchpad(*index, touch);
    REQUIRE(hp::pumpUntil([&] { return seen.frames.load() >= 2; }));
    const std::uint32_t after = uptimeMs();
    CHECK(seen.last.load() - before <= after - before);
}

namespace {

// Waits on the clock alone: the Qt loop does not run, so nothing queued to this thread can land.
bool waitWithoutQt(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) { return false; }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A Moonlight binding for `padId`: the host and number its feedback names the pad by.
dish::SlotRouter::MoonlightRoutes moonlightBinding(const std::string& host,
                                                   std::uint8_t controllerNumber) {
    dish::SlotRouter::MoonlightRoutes routes;
    routes.report = [](const GamepadInputProcessor::DeviceState&) {};
    routes.hostUuid = host;
    routes.controllerNumber = controllerNumber;
    return routes;
}

using MicSender = dish::SlotRouter::MicSender;

constexpr std::array<std::uint8_t, 5> kMicOpus{0xF8, 0x01, 0x02, 0x03, 0x04};
constexpr std::size_t kMicHeaderBytes = 3;

// The mic frames the one session's satellite saw: ctrlIdx, seq and the Opus bytes, each checked
// whole on its receive thread.
struct MicSeen {
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint32_t> lastSeq{0};
};

hp::HotPathRig::HandlerFor micInto(MicSeen& seen) {
    return [&seen](int /*session*/) {
        return [&seen](std::uint16_t type, const std::uint8_t* body, std::size_t len,
                       hp::Clock::time_point /*arrived*/) {
            if (type != dish::proto::kMsgMicAudio) { return; }
            const bool whole =
                len == kMicHeaderBytes + kMicOpus.size() && body[0] == 0 &&
                std::memcmp(body + kMicHeaderBytes, kMicOpus.data(), kMicOpus.size()) == 0;
            if (!whole) { seen.torn.fetch_add(1); }
            if (len >= kMicHeaderBytes) { seen.lastSeq.store(hp::readBe16(body + 1)); }
            seen.frames.fetch_add(1);
        };
    };
}

bool sendMic(const MicSender& mic, std::uint16_t seq) {
    return mic(seq, kMicOpus.data(), kMicOpus.size());
}

} // namespace

TEST_CASE("a Moonlight host's rumble reaches its pad from the control thread, without the GUI",
          "[slot-router][moonlight]") {
    Seen seen;
    hp::HotPathRig rig(1, countInto(seen));
    REQUIRE(rig.startBridge());
    const hp::VirtualPad pad("Router Moonlight pad");
    REQUIRE(pad.ok());
    const QString padId = pad.deviceId();
    REQUIRE(hp::pumpUntil([&] { return !rig.bridge().devices().isEmpty(); }));
    rig.router().setMoonlightRoutes(padId, moonlightBinding("host-a", 2));
    rig.router().republish({padId});

    std::thread control([&rig] { rig.router().moonlightRumble("host-a", 2, 0x4000, 0x2000); });
    control.join();
    CHECK(waitWithoutQt([&] { return pad.lastRumble() == ((0x4000U << 16) | 0x2000U); }));

    // Another number, or another host, names no pad of this binding.
    std::thread strangers([&rig] {
        rig.router().moonlightRumble("host-a", 1, 0x7000, 0x7000);
        rig.router().moonlightRumble("host-b", 2, 0x7000, 0x7000);
    });
    strangers.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(kQuietMs));
    CHECK(pad.lastRumble() == ((0x4000U << 16) | 0x2000U));

    rig.rumbleSwitch().setEnabled(padId, false);
    rig.router().republish({padId});
    std::thread switchedOff([&rig] { rig.router().moonlightRumble("host-a", 2, 0x5000, 0x5000); });
    switchedOff.join();
    CHECK(waitWithoutQt([&] { return pad.lastRumble() == 0U; }));

    rig.router().setMoonlightRoutes(padId, {});
}

TEST_CASE("a Moonlight host's light-bar colour lands only while it follows the game",
          "[slot-router][moonlight]") {
    Seen seen;
    hp::HotPathRig rig(1, countInto(seen));
    REQUIRE(rig.startBridge());
    const hp::VirtualPad pad("Router Moonlight light-bar pad");
    REQUIRE(pad.ok());
    const QString padId = pad.deviceId();
    REQUIRE(hp::pumpUntil([&] { return !rig.bridge().devices().isEmpty(); }));
    rig.router().setMoonlightRoutes(padId, moonlightBinding("host-a", 0));
    rig.router().republish({padId});

    rig.features().setLightbarMode(dish::LightbarMode::Off);
    std::thread off([&rig] { rig.router().moonlightLightbar("host-a", 0, 0x10, 0x20, 0x30); });
    off.join();
    CHECK(rig.router().lightbarsRouted() == 0);

    rig.features().setLightbarMode(dish::LightbarMode::FollowGame);
    std::thread follow([&rig] { rig.router().moonlightLightbar("host-a", 0, 0x10, 0x20, 0x30); });
    follow.join();
    CHECK(waitWithoutQt([&] { return rig.router().lightbarsRouted() >= 1; }));

    rig.router().setMoonlightRoutes(padId, {});
}

TEST_CASE("a slot's mic frames go where the published table routes the slot", "[slot-router]") {
    MicSeen seen;
    hp::HotPathRig rig(1, micInto(seen));
    REQUIRE(rig.connectAll());
    const QString slot = QStringLiteral("mic-pad");
    const QString lonely = QStringLiteral("unbound-mic-pad");
    REQUIRE(rig.bind(slot, 0));
    const MicSender mic = rig.router().micSenderForSlot(slot);
    const MicSender unbound = rig.router().micSenderForSlot(lonely);
    REQUIRE(mic != nullptr);
    REQUIRE(unbound != nullptr);

    CHECK_FALSE(sendMic(mic, 1));

    rig.router().republish({slot, lonely});
    CHECK(sendMic(mic, 2));
    REQUIRE(hp::pumpUntil([&] { return seen.frames.load() >= 1; }));
    CHECK(seen.lastSeq.load() == 2);
    CHECK_FALSE(sendMic(unbound, 3));

    rig.router().republish({});
    CHECK_FALSE(sendMic(mic, 4));
    CHECK(staysAt(seen.frames, 1));
    CHECK(seen.torn.load() == 0);
}

TEST_CASE("republishing under a mic stream never pulls a table from under it, nor tears a frame",
          "[slot-router]") {
    MicSeen seen;
    hp::HotPathRig rig(1, micInto(seen));
    REQUIRE(rig.connectAll());
    const QString slot = QStringLiteral("talking-pad");
    REQUIRE(rig.bind(slot, 0));
    rig.router().republish({slot});
    const MicSender mic = rig.router().micSenderForSlot(slot);
    REQUIRE(mic != nullptr);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> sent{0};
    std::thread capture([&] {
        const dish::thread::ScopedThreadRole role(dish::thread::ThreadRole::AudioCapture);
        for (std::uint16_t seq = 0; !stop.load(std::memory_order_relaxed); ++seq) {
            if (sendMic(mic, seq)) { sent.fetch_add(1); }
        }
    });
    for (int round = 0; round < 300; ++round) {
        rig.router().republish(round % 2 == 0 ? QStringList{} : QStringList{slot});
    }
    rig.router().republish({slot});
    const bool streamed = hp::pumpUntil([&] { return seen.frames.load() > 0; });
    stop.store(true);
    capture.join();
    CHECK(streamed);
    CHECK(sent.load() > 0);
    CHECK(seen.torn.load() == 0);
}

TEST_CASE("a mic sender kept past its router sends nothing", "[slot-router]") {
    MicSeen seen;
    MicSender mic;
    {
        hp::HotPathRig rig(1, micInto(seen));
        REQUIRE(rig.connectAll());
        const QString slot = QStringLiteral("parting-pad");
        REQUIRE(rig.bind(slot, 0));
        rig.router().republish({slot});
        mic = rig.router().micSenderForSlot(slot);
        REQUIRE(mic != nullptr);
        CHECK(sendMic(mic, 1));
    }
    CHECK_FALSE(sendMic(mic, 2));
}

TEST_CASE("a mic sender's participant comes back when its last copy goes", "[slot-router]") {
    MicSeen seen;
    hp::HotPathRig rig(1, micInto(seen));
    std::vector<MicSender> senders;
    for (std::size_t i = 0; i < dish::SlotRouter::kMicSenders; ++i) {
        senders.push_back(rig.router().micSenderForSlot(QStringLiteral("pad-%1").arg(i)));
        REQUIRE(senders.back() != nullptr);
    }
    CHECK(rig.router().micSenderForSlot(QStringLiteral("one-too-many")) == nullptr);

    {
        const MicSender copy = senders.back();
        senders.pop_back();
        CHECK(rig.router().micSenderForSlot(QStringLiteral("held-by-a-copy")) == nullptr);
    }
    CHECK(rig.router().micSenderForSlot(QStringLiteral("given-back")) != nullptr);
}

namespace {

constexpr std::uint64_t kPouringRumbles = 1000;

bool opened(const dish::input::SDLGamepadBridge& bridge, const QString& id) {
    const auto devices = bridge.devices();
    return std::any_of(
        devices.begin(), devices.end(),
        [&id](const dish::input::SDLGamepadBridge::Device& d) { return d.id == id; });
}

} // namespace

TEST_CASE("once every session is closed, feedback still pouring in never reaches the router",
          "[slot-router]") {
    Seen seen;
    hp::HotPathRig rig(2, countInto(seen));
    REQUIRE(rig.startBridge());
    const hp::VirtualPad pad("Router teardown pad");
    REQUIRE(pad.ok());
    const QString padId = pad.deviceId();
    REQUIRE(hp::pumpUntil([&] { return opened(rig.bridge(), padId); }));
    REQUIRE(rig.claimDualSense());
    REQUIRE(rig.connectAll());
    const QString usbSlot = hp::FakeDualSenseGateway::slotId();
    REQUIRE(rig.bind(padId, 0));
    REQUIRE(rig.bind(usbSlot, 1));
    REQUIRE(hp::pumpUntil(
        [&] { return rig.satellite(0).knowsDish() && rig.satellite(1).knowsDish(); }));
    rig.router().republish({padId, usbSlot});

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> rounds{0};
    std::thread satellites([&] {
        for (std::uint16_t strong = 0x4000; !stop.load(std::memory_order_relaxed); ++strong) {
            sendRumble(rig.satellite(0), strong);
            sendRumble(rig.satellite(1), strong);
            rounds.fetch_add(1);
        }
    });
    const bool pouring = hp::pumpUntil([&] {
        return pad.rumbles() > 0 && rig.usbGateway().writes() > 0 &&
               rig.router().rumblesRouted() >= kPouringRumbles;
    });
    rig.manager().closeSessions();
    const std::uint64_t routed = rig.router().rumblesRouted();
    const std::uint64_t roundsAtClose = rounds.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(kQuietMs));
    const std::uint64_t roundsAfter = rounds.load();
    const std::uint64_t routedAfter = rig.router().rumblesRouted();
    stop.store(true);
    satellites.join();
    CHECK(pouring);
    CHECK(roundsAfter > roundsAtClose);
    CHECK(routedAfter == routed);
}
