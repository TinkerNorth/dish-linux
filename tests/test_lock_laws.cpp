// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "HotPathRig.h"

#include "core/model/Protocol.h"
#include "core/thread/TrackedMutex.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace dt = dish::thread;
namespace hp = dish::test::hotpath;
namespace lockstats = dish::thread::lockstats;

using namespace std::chrono_literals;

enum class Allowance : std::uint8_t { ByDesign, Debt };

constexpr const char* allowanceName(Allowance why) {
    return why == Allowance::ByDesign ? "by design" : "debt";
}

struct LockName {
    const char* lock;
    Allowance why;
};

struct LockPair {
    const char* first;
    const char* second;
    Allowance why;
};

// The ratchet. Each list is exactly what today's code does: a lock that appears and is not listed
// fails, and so does a listed lock that no longer appears. The hot-path work deletes entries.

constexpr std::array<LockName, 0> kSdlInputMayAcquire{};

constexpr std::array<LockName, 0> kUsbReaderMayAcquire{};

constexpr std::array<LockName, 0> kReceiveMayAcquire{};

constexpr std::array<LockName, 0> kAudioCaptureMayAcquire{};

// The Linux sendto is MSG_DONTWAIT and carries no marker. A Direct pad's hidraw write holds
// hid.write on the claim's output worker, off every role here, and the rig's fake gateway never
// reaches it.
constexpr std::array<LockPair, 0> kBlockingMayHold{};

constexpr std::array<LockPair, 0> kOrderMayNest{};

constexpr auto kTick = 1ms;
constexpr auto kMicPeriod = 20ms;
constexpr auto kFeedbackPeriod = 10ms;
constexpr auto kGuiPeriod = 100ms;
constexpr auto kMinRun = 1s;
constexpr int kRunLimitMs = 30'000;
constexpr std::uint32_t kLightbarEvery = 10;
constexpr std::uint32_t kSpeakerEvery = 2;
constexpr std::uint64_t kEnoughInput = 300;
constexpr std::uint64_t kEnoughMotion = 30;
constexpr std::uint64_t kEnoughFeedback = 50;
constexpr std::uint64_t kEnoughLightbars = 5;
constexpr std::uint64_t kEnoughMic = 10;
constexpr std::uint64_t kEnoughPadRumbles = 5;
constexpr std::uint64_t kEnoughWrites = 20;
constexpr int kStickBase = 4096;
constexpr std::uint32_t kStickSteps = 1000;
constexpr int kStickStride = 16;
constexpr std::uint32_t kRumbleBase = 0x4000;
constexpr std::uint32_t kRumbleSteps = 64;
constexpr std::size_t kMicBytes = 60;
constexpr std::uint8_t kOpusFill = 0xF8;

using NameSet = std::set<std::string>;
using PairSet = std::set<std::pair<std::string, std::string>>;

// What the loopback satellite of one session saw, counted on its receive thread.
struct Tally {
    std::atomic<std::uint64_t> input{0};
    std::atomic<std::uint64_t> motion{0};
    std::atomic<std::uint64_t> touchpad{0};
    std::atomic<std::uint64_t> battery{0};
    std::atomic<std::uint64_t> mic{0};
    std::atomic<std::uint64_t> pings{0};
};

hp::LoopbackSatellite::Handler countInto(Tally& tally) {
    return [&tally](std::uint16_t msgType, const std::uint8_t* /*body*/, std::size_t /*len*/,
                    hp::Clock::time_point /*arrived*/) {
        switch (msgType) {
        case dish::proto::kMsgInput:
            tally.input.fetch_add(1);
            break;
        case dish::proto::kMsgMotion:
            tally.motion.fetch_add(1);
            break;
        case dish::proto::kMsgTouchpad:
            tally.touchpad.fetch_add(1);
            break;
        case dish::proto::kMsgBattery:
            tally.battery.fetch_add(1);
            break;
        case dish::proto::kMsgMicAudio:
            tally.mic.fetch_add(1);
            break;
        case dish::proto::kMsgHeartbeat:
            tally.pings.fetch_add(1);
            break;
        default:
            break;
        }
    };
}

// One thread, named for the role it plays, running `step` once per period until destroyed.
class Worker {
  public:
    Worker(dt::ThreadRole role, std::chrono::milliseconds period,
           std::function<void(std::uint32_t)> step)
        : thread_([this, role, period, work = std::move(step)] {
              const dt::ScopedThreadRole named(role);
              for (std::uint32_t tick = 0; !stop_.load(); ++tick) {
                  work(tick);
                  std::this_thread::sleep_for(period);
              }
          }) {}

    ~Worker() {
        stop_.store(true);
        if (thread_.joinable()) { thread_.join(); }
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

  private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::int16_t stickValue(std::uint32_t tick) {
    return static_cast<std::int16_t>(kStickBase +
                                     static_cast<int>(tick % kStickSteps) * kStickStride);
}

dish::source::usb::UsbReport movingFingerReport(std::uint32_t tick) {
    auto report = hp::dualSenseReport(stickValue(tick));
    report.finger0X = stickValue(tick);
    return report;
}

bool padPresent(const dish::input::SDLGamepadBridge& bridge, const QString& id) {
    const auto devices = bridge.devices();
    return std::any_of(
        devices.begin(), devices.end(),
        [&id](const dish::input::SDLGamepadBridge::Device& d) { return d.id == id; });
}

// The satellite's side of every session at once: rumble each tick, a light-bar colour and a
// speaker frame every few, all addressed to the session's one controller. The strength moves each
// tick because SDL drops a rumble equal to the one in force and only resends it every 2 s.
void sendFeedback(hp::HotPathRig& rig, std::uint32_t tick) {
    std::array<std::uint8_t, 7> rumble{0, 0, 0, 0x20, 0x00, 0x00, 0x64};
    hp::writeBe16(rumble.data() + 1,
                  static_cast<std::uint16_t>(kRumbleBase + (tick % kRumbleSteps)));
    const std::array<std::uint8_t, 4> lightbar{0, static_cast<std::uint8_t>(tick), 0x10, 0x80};
    std::array<std::uint8_t, 4> speaker{0, 0, 0, kOpusFill};
    hp::writeBe16(speaker.data() + 1, static_cast<std::uint16_t>(tick));
    for (int s = 0; s < rig.sessionCount(); ++s) {
        hp::LoopbackSatellite& satellite = rig.satellite(s);
        satellite.sendToDish(dish::proto::kMsgRumble, rumble.data(), rumble.size());
        if (tick % kLightbarEvery == 0) {
            satellite.sendToDish(dish::proto::kMsgLightbar, lightbar.data(), lightbar.size());
        }
        if (tick % kSpeakerEvery == 0) {
            satellite.sendToDish(dish::proto::kMsgSpeakerAudio, speaker.data(), speaker.size());
        }
    }
}

// The GUI thread's share while a pad plays: the rebuild's routing republish and twin set, the
// device list, and the inspector's and the rate sampler's reads. Answers how much it read.
std::size_t guiTick(hp::HotPathRig& rig, const QStringList& slotIds) {
    rig.router().republish(slotIds);
    rig.bridge().setSuppressedDeviceIds({});
    auto read = static_cast<std::size_t>(rig.bridge().devices().size());
    for (const QString& slot : slotIds) {
        const std::string id = slot.toStdString();
        if (rig.processor().inspect(id).wire.has_value()) { ++read; }
        if (rig.processor().inputCounters(id).gamepadEvents > 0) { ++read; }
    }
    return read;
}

struct LawRun {
    lockstats::Snapshot snap;
    std::string failed;
};

struct Progress {
    const std::array<Tally, 2>& tallies;
    const hp::HotPathRig& rig;
    const hp::VirtualPad& pad;

    bool enough() const {
        return tallies[0].input.load() >= kEnoughInput && tallies[1].input.load() >= kEnoughInput &&
               tallies[1].motion.load() >= kEnoughMotion &&
               tallies[1].touchpad.load() >= kEnoughInput && tallies[0].mic.load() >= kEnoughMic &&
               tallies[0].pings.load() > 0 && tallies[1].pings.load() > 0 &&
               rig.satellite(0).acksSent() > 0 && rig.satellite(1).acksSent() > 0 &&
               pad.rumbles() >= kEnoughPadRumbles && rig.usbGateway().writes() >= kEnoughWrites &&
               rig.router().rumblesRouted() >= kEnoughFeedback &&
               rig.router().lightbarsRouted() >= kEnoughLightbars;
    }
};

// Every role at once over the real objects: the SDL thread on a virtual pad, a raw-HID read
// thread, two sessions' receive and heartbeat threads fed by loopback satellites, the mic, and the
// GUI's periodic reads. The snapshot covers the whole life: attach, steady state and teardown.
LawRun runTheHotPathsTogether() {
    const dt::ScopedThreadRole gui(dt::ThreadRole::Gui);
    lockstats::reset();
    LawRun run;
    std::array<Tally, 2> tallies;
    {
        hp::HotPathRig rig(2, [&tallies](int session) {
            return countInto(tallies[static_cast<std::size_t>(session)]);
        });
        if (!rig.startBridge()) {
            run.failed = "the SDL thread never initialised SDL";
            return run;
        }
        QString padId;
        {
            const hp::VirtualPad pad("Lock law pad");
            padId = pad.deviceId();
            if (!pad.ok() || !hp::pumpUntil([&] { return padPresent(rig.bridge(), padId); })) {
                run.failed = "the bridge never opened the virtual pad";
                return run;
            }
            if (!rig.claimDualSense()) {
                run.failed = "the USB manager never claimed the DualSense";
                return run;
            }
            if (!rig.connectAll()) {
                run.failed = "a session never went live";
                return run;
            }
            const QString usbSlot = hp::FakeDualSenseGateway::slotId();
            if (!rig.bind(padId, 0) || !rig.bind(usbSlot, 1)) {
                run.failed = "a slot was never applied";
                return run;
            }
            const QStringList slotIds{padId, usbSlot};
            rig.router().republish(slotIds);
            const auto mic = rig.router().micSenderForSlot(padId);
            if (!mic) {
                run.failed = "the bound pad has no mic sender";
                return run;
            }
            const Progress progress{tallies, rig, pad};
            std::size_t guiReads = 0;
            {
                const Worker stimulus(dt::ThreadRole::Other, kTick, [&pad](std::uint32_t tick) {
                    pad.setRightX(stickValue(tick));
                });
                const Worker reader(dt::ThreadRole::UsbReader, kTick, [&rig](std::uint32_t tick) {
                    rig.usbGateway().deliver(movingFingerReport(tick));
                });
                const Worker capture(
                    dt::ThreadRole::AudioCapture, kMicPeriod, [&mic](std::uint32_t tick) {
                        const std::array<std::uint8_t, kMicBytes> opus{kOpusFill};
                        mic(static_cast<std::uint16_t>(tick), opus.data(), opus.size());
                    });
                const Worker satellites(dt::ThreadRole::Other, kFeedbackPeriod,
                                        [&rig](std::uint32_t tick) { sendFeedback(rig, tick); });
                const auto started = hp::Clock::now();
                const bool done = hp::pumpUntil(
                    [&] {
                        guiReads += guiTick(rig, slotIds);
                        std::this_thread::sleep_for(kGuiPeriod);
                        return hp::Clock::now() - started >= kMinRun && guiReads > 0 &&
                               progress.enough();
                    },
                    kRunLimitMs);
                if (!done) { run.failed = "the run never reached every path"; }
            }
        }
        if (!hp::pumpUntil([&] { return !padPresent(rig.bridge(), padId); })) {
            run.failed = "the bridge never closed the detached pad";
        }
    }
    run.snap = lockstats::snapshot();
    return run;
}

NameSet acquiredBy(const lockstats::Snapshot& snap, dt::ThreadRole role) {
    NameSet names;
    for (const lockstats::LockStats& lock : snap.locks) {
        if (lock.role(role).touched()) { names.insert(lock.name); }
    }
    return names;
}

PairSet pairsOf(const std::vector<lockstats::NamePair>& pairs) {
    PairSet out;
    for (const lockstats::NamePair& pair : pairs) { out.insert({pair.first, pair.second}); }
    return out;
}

std::string joined(const NameSet& names) {
    std::ostringstream out;
    for (const std::string& name : names) { out << name << ' '; }
    return out.str();
}

std::string joined(const std::vector<std::string>& names) {
    std::ostringstream out;
    for (const std::string& name : names) { out << name << ' '; }
    return out.str();
}

std::string joined(const PairSet& pairs) {
    std::ostringstream out;
    for (const auto& [first, second] : pairs) { out << '(' << first << ", " << second << ") "; }
    return out.str();
}

void expectExactly(const std::string& law, const NameSet& observed,
                   std::span<const LockName> listed) {
    INFO(law << " observed: " << joined(observed));
    NameSet names;
    for (const LockName& entry : listed) { names.insert(entry.lock); }
    for (const std::string& lock : observed) {
        if (names.count(lock) == 0) { FAIL_CHECK(law << ": " << lock << " is not on the list"); }
    }
    for (const LockName& entry : listed) {
        if (observed.count(entry.lock) == 0) {
            FAIL_CHECK(law << ": " << entry.lock << " (" << allowanceName(entry.why)
                           << ") is listed but no longer observed; delete it");
        }
    }
    CHECK(observed.size() == listed.size());
}

void expectExactly(const std::string& law, const PairSet& observed,
                   std::span<const LockPair> listed) {
    INFO(law << " observed: " << joined(observed));
    PairSet pairs;
    for (const LockPair& entry : listed) { pairs.insert({entry.first, entry.second}); }
    for (const auto& [first, second] : observed) {
        if (pairs.count({first, second}) == 0) {
            FAIL_CHECK(law << ": (" << first << ", " << second << ") is not on the list");
        }
    }
    for (const LockPair& entry : listed) {
        if (observed.count({entry.first, entry.second}) == 0) {
            FAIL_CHECK(law << ": (" << entry.first << ", " << entry.second << ") ("
                           << allowanceName(entry.why)
                           << ") is listed but no longer observed; delete it");
        }
    }
    CHECK(observed.size() == listed.size());
}

bool findCycle(const PairSet& edges, std::vector<std::string>& cycle) {
    std::map<std::string, std::vector<std::string>> next;
    for (const auto& [from, to] : edges) { next[from].push_back(to); }
    std::map<std::string, int> state;
    std::vector<std::string> path;
    std::function<bool(const std::string&)> visit = [&](const std::string& node) {
        state[node] = 1;
        path.push_back(node);
        for (const std::string& to : next[node]) {
            if (state[to] == 1) {
                cycle.assign(std::find(path.begin(), path.end(), to), path.end());
                cycle.push_back(to);
                return true;
            }
            if (state[to] == 0 && visit(to)) { return true; }
        }
        path.pop_back();
        state[node] = 2;
        return false;
    };
    for (const auto& [from, to] : edges) {
        if (state[from] == 0 && visit(from)) { return true; }
    }
    return false;
}

LawRun wiredRun() {
    LawRun run = runTheHotPathsTogether();
    INFO(run.failed);
    REQUIRE(run.failed.empty());
    REQUIRE(run.snap.overflows == 0U);
    return run;
}

} // namespace

TEST_CASE("lock law 1: the SDL input thread takes exactly today's locks", "[lockstats][laws]") {
    const LawRun run = wiredRun();
    expectExactly("SdlInput law", acquiredBy(run.snap, dt::ThreadRole::SdlInput),
                  kSdlInputMayAcquire);
}

TEST_CASE("lock law 2: the raw-HID read thread takes exactly today's locks", "[lockstats][laws]") {
    const LawRun run = wiredRun();
    expectExactly("UsbReader law", acquiredBy(run.snap, dt::ThreadRole::UsbReader),
                  kUsbReaderMayAcquire);
}

TEST_CASE("lock law 3: the receive thread takes exactly today's locks", "[lockstats][laws]") {
    const LawRun run = wiredRun();
    expectExactly("Receive law", acquiredBy(run.snap, dt::ThreadRole::Receive), kReceiveMayAcquire);
}

TEST_CASE("lock law 4: the locks held across a blocking call are exactly today's",
          "[lockstats][laws]") {
    const LawRun run = wiredRun();
    expectExactly("blocking law", pairsOf(run.snap.blocking), kBlockingMayHold);
}

TEST_CASE("lock law 5: every nesting edge is today's, and the order has no cycle",
          "[lockstats][laws]") {
    const LawRun run = wiredRun();
    const PairSet edges = pairsOf(run.snap.edges);
    std::vector<std::string> cycle;
    const bool cyclic = findCycle(edges, cycle);
    INFO("cycle: " << joined(cycle));
    CHECK_FALSE(cyclic);
    expectExactly("order law", edges, kOrderMayNest);
}

TEST_CASE("lock law 6: the mic capture thread takes exactly today's locks", "[lockstats][laws]") {
    const LawRun run = wiredRun();
    expectExactly("AudioCapture law", acquiredBy(run.snap, dt::ThreadRole::AudioCapture),
                  kAudioCaptureMayAcquire);
}
