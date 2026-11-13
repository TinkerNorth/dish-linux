// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "core/thread/Seqlock.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace dish::input {

// Converts raw gamepad values into the XUSB report format the Satellite
// server expects. Pure logic; no SDL dependency.
//
// Every device has a record in a fixed table, found by the dense index add() hands out. The thread
// that reads a device (the SDL thread, a raw-HID read thread) publishes by that index with no lock
// and no allocation; the GUI copies a record out through its seqlock. The senders are set before
// any input thread starts and never change afterwards.
class GamepadInputProcessor {
  public:
    // Wire values — do not renumber.
    struct Buttons {
        static constexpr std::uint16_t kDpadUp = 0x0001;
        static constexpr std::uint16_t kDpadDown = 0x0002;
        static constexpr std::uint16_t kDpadLeft = 0x0004;
        static constexpr std::uint16_t kDpadRight = 0x0008;
        static constexpr std::uint16_t kStart = 0x0010;
        static constexpr std::uint16_t kBack = 0x0020;
        static constexpr std::uint16_t kLeftThumb = 0x0040;
        static constexpr std::uint16_t kRightThumb = 0x0080;
        static constexpr std::uint16_t kLeftShoulder = 0x0100;
        static constexpr std::uint16_t kRightShoulder = 0x0200;
        static constexpr std::uint16_t kGuide = 0x0400;
        static constexpr std::uint16_t kA = 0x1000;
        static constexpr std::uint16_t kB = 0x2000;
        static constexpr std::uint16_t kX = 0x4000;
        static constexpr std::uint16_t kY = 0x8000;
    };

    using DeviceId = std::string;
    using DeviceIndex = std::uint32_t;
    static constexpr std::size_t kMaxDevices = 64;
    // Longer ids are refused by add().
    static constexpr std::size_t kMaxIdBytes = 63;

    // Runs inline on the caller's thread (normally the SDL gamepad thread);
    // the hot path deliberately never hops threads.
    using ReportSender =
        std::function<void(const DeviceId& id, std::uint16_t wButtons, std::uint8_t lt,
                           std::uint8_t rt, std::int16_t lx, std::int16_t ly, std::int16_t rx,
                           std::int16_t ry, std::uint32_t extButtons)>;

    // Rate-limited per device to kMotionRateLimitHz before it reaches the
    // sender.
    struct MotionSample {
        std::int16_t gyroX = 0;
        std::int16_t gyroY = 0;
        std::int16_t gyroZ = 0;
        std::int16_t accelX = 0;
        std::int16_t accelY = 0;
        std::int16_t accelZ = 0;
    };
    using MotionSender =
        std::function<void(const DeviceId& id, std::int16_t gyroX, std::int16_t gyroY,
                           std::int16_t gyroZ, std::int16_t accelX, std::int16_t accelY,
                           std::int16_t accelZ, std::uint32_t timestampDeltaUs)>;

    // Never coalesced: MSG_BATTERY is a fixed 30 s heartbeat, so an unchanged
    // value must still reach the wire for a dropped packet to self-heal. The
    // SDL bridge's poll gate owns the cadence.
    struct BatterySample {
        std::uint8_t level = 0xFF;
        std::uint8_t status = 0;
    };
    using BatterySender =
        std::function<void(const DeviceId& id, std::uint8_t level, std::uint8_t status)>;

    // Forwarded as published: a raw-HID pad's publisher gates its report stream first
    // (Input/TouchGate.h). An eventTimeMs of 0 is stamped at the forward.
    struct TouchpadSample {
        bool finger0Active = false;
        std::uint8_t finger0Id = 0;
        std::int16_t finger0X = 0;
        std::int16_t finger0Y = 0;
        bool finger1Active = false;
        std::uint8_t finger1Id = 0;
        std::int16_t finger1X = 0;
        std::int16_t finger1Y = 0;
        bool buttonPressed = false;
        // The pointer frame's right button and wheel (protocol 2): a Steam
        // Controller's trackpads composed as a mouse. The wheel is an event, so
        // a frame that carries one is never "the same" as the frame before.
        bool rightPressed = false;
        std::int16_t scrollV = 0;
        std::uint32_t eventTimeMs = 0;
        bool sameTouch(const TouchpadSample& o) const {
            return finger0Active == o.finger0Active && finger0Id == o.finger0Id &&
                   finger0X == o.finger0X && finger0Y == o.finger0Y &&
                   finger1Active == o.finger1Active && finger1Id == o.finger1Id &&
                   finger1X == o.finger1X && finger1Y == o.finger1Y &&
                   buttonPressed == o.buttonPressed && rightPressed == o.rightPressed &&
                   scrollV == o.scrollV;
        }
    };
    using TouchpadSender = std::function<void(const DeviceId& id, const TouchpadSample& sample)>;
    using NanoTime = std::function<std::int64_t()>;

    // Max forwarded MSG_MOTION rate per controller; faster samples are dropped.
    static constexpr std::uint32_t kMotionRateLimitHz = 250;
    static constexpr std::uint64_t kMotionMinIntervalUs =
        1'000'000ULL / kMotionRateLimitHz; // 4000 µs

    struct DeviceState {
        std::uint16_t wButtons = 0;
        std::uint8_t lt = 0;
        std::uint8_t rt = 0;
        std::int16_t lx = 0;
        std::int16_t ly = 0;
        std::int16_t rx = 0;
        std::int16_t ry = 0;
        // The protocol 4 extended-buttons word (proto::kExtButton*).
        std::uint32_t extButtons = 0;
        bool operator==(const DeviceState& o) const {
            return wButtons == o.wButtons && lt == o.lt && rt == o.rt && lx == o.lx && ly == o.ly &&
                   rx == o.rx && ry == o.ry && extButtons == o.extButtons;
        }
    };

    // Values at or below the flat are zeroed before the report leaves the
    // processor. SDL2 exposes no OS-level per-device flat, so SDLGamepadBridge
    // installs a default on attach.
    struct Deadzones {
        std::int16_t stickFlat = 0;
        std::uint8_t triggerFlat = 0;
        bool operator==(const Deadzones& o) const {
            return stickFlat == o.stickFlat && triggerFlat == o.triggerFlat;
        }
    };

    // `events` counts every report published, `sends` the ones that went on to the sender.
    struct TelemetrySnapshot {
        int events = 0;
        int sends = 0;
        std::uint64_t totalSent = 0;
    };

    // The seam InputRateStore samples to derive Hz. `motion` counts *forwarded*
    // samples, not attempts, or a throttled stream would over-report. Monotonic
    // within a device's lifetime; remove() drops the entry so a re-attach
    // re-baselines instead of appearing to leap forward.
    struct InputRateCounters {
        std::uint64_t gamepadEvents = 0;
        std::uint64_t motionEvents = 0;
    };

    // 0/0 if the device never emitted. Off the hot path (~1 Hz, Qt main thread).
    InputRateCounters inputCounters(const DeviceId& id) const;

    // What the input inspector reads for one device: its last report as it went
    // on the wire (after the dead zones) and as the pad sent it, and the last
    // motion sample and touch frame it forwarded. Each is empty until the
    // device has produced one.
    struct Inspection {
        std::optional<DeviceState> wire;
        std::optional<DeviceState> raw;
        std::optional<MotionSample> motion;
        std::optional<TouchpadSample> touchpad;
    };

    // Polled by the open inspector, off the hot path; copies the record out through its seqlock.
    Inspection inspect(const DeviceId& id) const;

    // Reports that actually moved something, across every device. Unlike the
    // per-device rate counters this excludes an untouched pad's idle stream, so
    // it answers "is anyone playing" rather than "is anything arriving".
    // Process-monotonic: a device leaving does not roll it back.
    std::uint64_t actuationCount() const { return actuations_.load(std::memory_order_relaxed); }

    // Where the app's reports leave, keyed by the device's index, which is what a published route
    // table is indexed by. Called on the device's own thread.
    class InputSink {
      public:
        virtual ~InputSink() = default;
        virtual void report(DeviceIndex index, const DeviceId& id, const DeviceState& state) = 0;
        virtual void motion(DeviceIndex index, const DeviceId& id, const MotionSample& sample,
                            std::uint32_t timestampDeltaUs) = 0;
        virtual void battery(DeviceIndex index, const DeviceId& id,
                             const BatterySample& sample) = 0;
        virtual void touchpad(DeviceIndex index, const DeviceId& id,
                              const TouchpadSample& sample) = 0;

      protected:
        InputSink() = default;
        InputSink(const InputSink&) = default;
        InputSink& operator=(const InputSink&) = default;
        InputSink(InputSink&&) = default;
        InputSink& operator=(InputSink&&) = default;
    };

    // Set before any input thread starts; read by them without a lock afterwards. The senders are
    // plain hooks (the tests' window onto what leaves); the app routes through the sink.
    void setSink(InputSink* sink) { sink_ = sink; }
    void setReportSender(ReportSender sender);
    void setMotionSender(MotionSender sender);
    void setBatterySender(BatterySender sender);
    void setTouchpadSender(TouchpadSender sender);
    void setNanoTime(NanoTime nanoTime);
    std::int64_t nanoTime() const { return nanoTime_(); }

    // The device's index, the one it already has or a free one, without a lock. Empty when the
    // table is full or the id is too long. One thread introduces a given id.
    std::optional<DeviceIndex> add(const DeviceId& id);
    // The device's index if it has one, without making one. Any thread, no lock.
    std::optional<DeviceIndex> find(const DeviceId& id) const;
    // Frees the device's record. Called once whoever publishes for the device has stopped.
    void remove(const DeviceId& id);
    void setDeadzones(const DeviceId& id, const Deadzones& dz);

    // The hot path, on the device's own thread: no lock, no allocation.
    // publish forwards the report only when it differs, after the dead zones, from the last one
    // forwarded for the device; the first report after the device appears, or after remove(),
    // always goes.
    void publish(DeviceIndex index, const DeviceState& state);
    // False when the rate limiter dropped the sample.
    bool publishMotion(DeviceIndex index, const MotionSample& sample);
    // Test seam taking an explicit `now` (microseconds, any monotonic basis).
    bool publishMotionAt(DeviceIndex index, const MotionSample& sample, std::uint64_t nowUs);
    void publishBattery(DeviceIndex index, const BatterySample& sample);
    void publishTouchpad(DeviceIndex index, const TouchpadSample& sample);

    // The same by id, for callers off the hot path: add() and then the index form.
    void publish(const DeviceId& id, const DeviceState& state);
    bool publishMotion(const DeviceId& id, const MotionSample& sample);
    bool publishMotionAt(const DeviceId& id, const MotionSample& sample, std::uint64_t nowUs);
    void publishBattery(const DeviceId& id, const BatterySample& sample);
    void publishTouchpad(const DeviceId& id, const TouchpadSample& sample);

    // Sends a neutral report to the report sender for every device, whatever it last sent; each
    // device's latch then holds the zeros, so its next report goes out unless it is neutral too.
    void zeroAndSendAll();
    TelemetrySnapshot drainTelemetry();

  private:
    struct IdText {
        std::array<char, kMaxIdBytes> chars{};
        std::uint8_t length = 0;
    };

    // `lastUs` and `lastSample` are the last *emitted* sample. `hasEmitted` is a
    // separate flag, not a `lastUs == 0` sentinel: a monotonic clock can
    // legitimately read 0, which would make the second sample look like another
    // first sample.
    struct MotionGate {
        std::uint64_t lastUs = 0;
        bool hasEmitted = false;
    };

    static constexpr std::uint32_t kFree = 0;
    static constexpr std::uint32_t kClaimed = 1;
    static constexpr std::uint32_t kLive = 2;
    static constexpr std::uint32_t kSeenReport = 1U << 0;
    static constexpr std::uint32_t kSeenMotion = 1U << 1;
    static constexpr std::uint32_t kSeenTouchpad = 1U << 2;

    struct Record {
        // The registry: Free, then Claimed while one thread writes the id, then Live. The id text
        // is what lookups compare, through idSeq; `id` is what the senders get, written only while
        // Claimed and read only by the device's own thread.
        std::atomic<std::uint32_t> state{kFree};
        thread::Seqlock idSeq;
        thread::AtomicWords<IdText> idText;
        DeviceId id;
        // Bumped by add and remove: the device's thread starts its own state over when it sees a
        // value other than the one it last saw. zeroGen is zeroAndSendAll's: the latch then holds
        // the zeros it sent.
        std::atomic<std::uint32_t> plugGen{0};
        std::atomic<std::uint32_t> zeroGen{0};
        std::atomic<std::uint32_t> deadzones{0};
        // Written by the device's thread inside `seq`, copied out by inspect().
        thread::Seqlock seq;
        thread::AtomicWords<DeviceState> wire;
        thread::AtomicWords<DeviceState> raw;
        thread::AtomicWords<MotionSample> motion;
        thread::AtomicWords<TouchpadSample> touchpad;
        std::atomic<std::uint32_t> seen{0};
        std::atomic<std::uint64_t> gamepadEvents{0};
        std::atomic<std::uint64_t> motionEvents{0};
        // The device's thread's own.
        std::uint32_t ownGen = 0;
        std::uint32_t ownZeroGen = 0;
        bool forwarded = false;
        DeviceState lastForwarded{};
        // The baseline actuation is measured against. Distinct from the latch, which follows
        // every forwarded report: a stick drifting under the epsilon never moves this, so a slow
        // deliberate push still accumulates against it and trips.
        DeviceState activityRef{};
        MotionGate gate{};
        std::optional<TouchpadSample> lastTouchpad;
    };

    bool idMatches(const Record& r, const DeviceId& id) const;
    Record* recordAt(DeviceIndex index);
    // The device's thread, catching up with a plugGen the cold side moved.
    static void syncOwner(Record& r);
    static std::int64_t steadyNanoTime();

    std::array<Record, kMaxDevices> records_;
    InputSink* sink_ = nullptr;
    ReportSender sender_;
    MotionSender motionSender_;
    BatterySender batterySender_;
    TouchpadSender touchpadSender_;
    NanoTime nanoTime_ = steadyNanoTime;

    std::atomic<std::uint64_t> actuations_{0};
    std::atomic<int> telEvents_{0};
    std::atomic<int> telSends_{0};
    std::atomic<std::uint64_t> telTotalSent_{0};
};

std::int16_t scaleAxis(float v, float maxMagnitude);
std::uint8_t scaleTrigger(float v);

// Free function so tests can pin the arithmetic without the lock plumbing.
GamepadInputProcessor::DeviceState applyDeadzones(const GamepadInputProcessor::DeviceState& state,
                                                  const GamepadInputProcessor::Deadzones& dz);

// Deadzones are only installed for SDL-enumerated devices, so a USB-direct slot
// arrives unfiltered and its resting sticks dither by a wire LSB (257 counts)
// every report. Equality alone would read that pad as played-with forever, so
// the axes need a threshold; buttons are exact.
inline constexpr std::int16_t kActuationStickEpsilon = 1536;
inline constexpr std::uint8_t kActuationTriggerEpsilon = 16;

// Free function for the same reason as applyDeadzones.
bool isActuation(const GamepadInputProcessor::DeviceState& ref,
                 const GamepadInputProcessor::DeviceState& next);

} // namespace dish::input
