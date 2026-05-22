// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Encrypted UDP session to a single Satellite server. The Linux analogue of
// dish-mac/Network/SatelliteClient.swift and dish-android/satellite_jni.cpp.
//
// Owns one raw POSIX UDP socket, the ChaCha20-Poly1305 key/token, a monotonic
// nonce counter, a heartbeat sender thread and an ACK receive thread.
//
// Thread-safety:
//   * sendReport() is called directly from the SDL gamepad thread for minimum
//     latency and is lock-free except for the duration of one ::sendto.
//   * Public lifecycle calls (open/close, setConnectionParams, start/stop) are
//     expected to be invoked from a single owner thread (typically the Qt main
//     thread).

#pragma once

#include "Util/AtomicCounter.h"

#include <netinet/in.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace dish::net {

class SatelliteClient {
  public:
    static constexpr std::uint16_t kMsgGamepadData = 0x0001;
    static constexpr std::uint16_t kMsgHeartbeatPing = 0x0002;
    static constexpr std::uint16_t kMsgHeartbeatAck = 0x0003;
    static constexpr std::uint16_t kMsgControllerAdd = 0x0004;
    static constexpr std::uint16_t kMsgControllerRemove = 0x0005;
    static constexpr std::uint16_t kMsgControllerAck = 0x0006;
    static constexpr std::uint16_t kMsgServerStatus = 0x0007;
    static constexpr std::uint16_t kMsgControllerType = 0x0008;
    static constexpr std::uint16_t kMsgRumble = 0x0009;
    static constexpr std::uint16_t kMsgMotion = 0x000A;
    static constexpr std::uint16_t kMsgBattery = 0x000B;
    static constexpr std::uint16_t kMsgTouchpad = 0x000C;
    static constexpr std::uint16_t kMsgLightbar = 0x000D;
    // Mid-session capability update. Same payload shape as the caps field of
    // MSG_CONTROLLER_ADD — ctrlIdx(1) + caps(2 BE) = 3 bytes — but applied to an
    // already-registered controller without an unplug/replug. Sent when the
    // per-controller capability word would differ from what was last
    // advertised; a pre-extension satellite drops the packet silently (see
    // satellite/src/net/inner_dispatch.cpp), so it's forward-compatible. Today
    // dish-linux has no in-session motion toggle, so this is wired but unused
    // outside of tests — see WifiConnection::sendCapsUpdate.
    static constexpr std::uint16_t kMsgControllerCapsUpdate = 0x000E;

    // Controller-add capability bits. Bits 0x01 / 0x02 are documented in
    // satellite/docs/protocol.md (analog triggers, rumble). Bit 0x04 is the
    // motion (gyro + accel) cap — the satellite uses it to advertise motion
    // on the virtual device where the backend supports a motion surface.
    static constexpr std::uint16_t kCapAnalogTriggers = 0x0001;
    static constexpr std::uint16_t kCapRumble = 0x0002;
    static constexpr std::uint16_t kCapMotion = 0x0004;
    // CAP_LIGHTBAR (Task 1.4). Advertised per-controller in MSG_CONTROLLER_ADD
    // only when the bound physical pad has an addressable RGB LED (DualSense /
    // DualShock 4). A satellite that sees this bit sends lightbar colour via
    // the dedicated MSG_LIGHTBAR (0x000D) stream.
    static constexpr std::uint16_t kCapLightbar = 0x0008;

    // Optional motion-flags byte appended to the MSG_CONTROLLER_ACK payload by
    // a post-extension satellite (wire payload-length becomes 5 instead of 4).
    // Lets the dish learn the receiver's motion-sink facts at the moment they
    // are definitively known (plug-in time) without polling /api/connections.
    // Bit 0 — backend has an IMU surface for the chosen controller type
    //         (e.g. ViGEm DS4 on Windows, uinput PS-class on Linux). False on
    //         macOS and on any backend that hasn't shipped a motion adapter.
    // Bit 1 — backend successfully created the per-serial IMU sink at plug-in
    //         time. False distinguishes "kernel rejected the motion node" from
    //         "no game has subscribed yet" — the dish should surface the
    //         former to the user (a CAP_MOTION advertisement we won't be able
    //         to honour end-to-end).
    // A pre-extension satellite omits the byte (msgLen == 4); the dish stays
    // at the -1 "unknown" sentinel so an old satellite never reads as a
    // permanent failure.
    static constexpr std::uint8_t kAckMotionFlagSinkSupportedForType = 0x01;
    static constexpr std::uint8_t kAckMotionFlagBackendOk = 0x02;

    // Battery wire constants (satellite/src/core/types.h mirrors).
    static constexpr std::uint8_t kBatteryLevelUnknown = 0xFF;
    static constexpr std::uint8_t kBatteryStatusUnknown = 0;
    static constexpr std::uint8_t kBatteryStatusDischarging = 1;
    static constexpr std::uint8_t kBatteryStatusCharging = 2;
    static constexpr std::uint8_t kBatteryStatusFull = 3;
    static constexpr std::uint8_t kBatteryStatusWired = 4;

    static constexpr std::uint32_t kHeartbeatIntervalMs = 2000;
    static constexpr int kHeartbeatMissMax = 5;

    SatelliteClient();
    ~SatelliteClient();

    SatelliteClient(const SatelliteClient&) = delete;
    SatelliteClient& operator=(const SatelliteClient&) = delete;
    SatelliteClient(SatelliteClient&&) = delete;
    SatelliteClient& operator=(SatelliteClient&&) = delete;

    // Returns true on success. On failure the client is left closed.
    bool openSocket(const std::string& ip, int port);
    void closeSocket();

    // Install the post-pair token (4B) + shared key (32B). Resets counter/ACK.
    void setConnectionParams(const std::array<std::uint8_t, 4>& token,
                             const std::array<std::uint8_t, 32>& key);

    // Hot path: called directly from the SDL input thread.
    void sendReport(int controllerIndex, std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt,
                    std::int16_t lx, std::int16_t ly, std::int16_t rx, std::int16_t ry);

    void controllerAdd(int index, std::uint16_t capabilities);
    void controllerRemove(int index);
    void sendControllerType(int index, int type);

    // Push a fresh capability word for an already-registered controller. Wire
    // payload mirrors the caps field of MSG_CONTROLLER_ADD: ctrlIdx(1) +
    // caps(2 BE) = 3 bytes. A pre-extension satellite drops the packet
    // silently (see satellite/src/net/inner_dispatch.cpp), so the call is
    // forward-compatible. Today dish-linux has no per-slot motion toggle UI,
    // so this is wired through but unused at runtime; it exists so a future
    // per-controller motion-enabled toggle can land without re-touching the
    // wire layer. Mirrors satellite_jni.cpp::sendControllerCapsUpdate on
    // dish-android.
    void sendCapsUpdate(int index, std::uint16_t capabilities);

    void resetControllerAck() {
        lastControllerAck_.store(-1, std::memory_order_relaxed);
        // Reset the motion-flags sentinel in lockstep with the ACK reset so a
        // freshly-reset slot can't spuriously read its predecessor's
        // "backend rejected motion" flag — same invariant as the JNI side.
        lastControllerAckMotionFlags_.store(-1, std::memory_order_relaxed);
    }

    // Pure helper: fold the per-controller CAP_LIGHTBAR (0x0008) bit into a
    // base capability word. Returns `base` unchanged when the bound pad has
    // no addressable RGB LED, and `base | kCapLightbar` when it does. Kept
    // public + static so the cap-advertisement rule is unit-testable without
    // a live socket — see WifiConnection::registerController for the caller.
    static std::uint16_t withLightbarCapability(std::uint16_t base, bool hasLightbar) {
        return static_cast<std::uint16_t>(base | (hasLightbar ? kCapLightbar : 0));
    }

    // Pure helper: fold the per-controller CAP_MOTION (0x0004) bit into a
    // base capability word. Returns `base` unchanged when the bound pad has
    // no gyro / accelerometer, and `base | kCapMotion` when it does. The
    // motion analogue of withLightbarCapability — kept public + static for
    // the same unit-test reasons.
    static std::uint16_t withMotionCapability(std::uint16_t base, bool hasMotion) {
        return static_cast<std::uint16_t>(base | (hasMotion ? kCapMotion : 0));
    }

    // Forward a single IMU sample. Axes follow the satellite's
    // Cemuhook-compatible convention (right-handed, +X right, +Y up,
    // +Z toward player).
    //
    // Load-bearing assumption: no caller applies a manufacturer rotation
    // matrix, and none needs to. SDL2 already normalises HIDAPI controllers
    // (DualSense / DS4 / Switch Pro) into exactly this right-handed frame
    // internally, so the int16 triples handed in here are already in wire
    // orientation. The protocol's "senders apply the rotation matrix" rule
    // (protocol.md §0x000A) is satisfied by SDL on our behalf — do NOT add a
    // rotation step here or it would double-apply.
    //
    // Scale: gyro int16 LSB = 2000/32767 deg/s; accel int16 LSB = 4/32767 g.
    // See satellite/docs/protocol.md §0x000A for the canonical reference.
    //
    // `timestampDeltaUs` is microseconds since the previous motion packet
    // for the same controller on this connection. 0 on the very first packet.
    //
    // Hot path: called from the SDL sensor-update thread.
    void sendMotion(int controllerIndex, std::int16_t gyroX, std::int16_t gyroY, std::int16_t gyroZ,
                    std::int16_t accelX, std::int16_t accelY, std::int16_t accelZ,
                    std::uint32_t timestampDeltaUs);

    // Forward a single battery sample. `level` is 0..100 inclusive, or
    // `kBatteryLevelUnknown` (0xFF). `status` is one of the kBatteryStatus*
    // constants. Senders that have no battery information at all MUST NOT
    // call this; partial readers (status-only) should send level=0xFF.
    void sendBattery(int controllerIndex, std::uint8_t level, std::uint8_t status);

    // Forward a touchpad sample (MSG_TOUCHPAD, 0x000C — DualSense / DS4).
    // Up to two fingers; `fingerNActive` gates whether that finger's id +
    // coordinates are meaningful. Coordinates are normalised int16
    // (-32768..32767) on both axes so the wire is resolution-independent.
    // `buttonPressed` is the clickable-pad switch.
    //
    // Hot path: called from the SDL touchpad-event thread.
    void sendTouchpad(int controllerIndex, bool finger0Active, std::uint8_t finger0Id,
                      std::int16_t finger0X, std::int16_t finger0Y, bool finger1Active,
                      std::uint8_t finger1Id, std::int16_t finger1X, std::int16_t finger1Y,
                      bool buttonPressed);

    // Pure encoder for the MSG_MOTION inner payload (after the 4-byte
    // type+length header). The wire layout is host-LE for the int16 / uint32
    // fields, matching satellite/src/core/types.h::MotionReport. Exposed
    // statically so unit tests can pin the byte format without bringing
    // up a live socket — the same pattern that parseRumbleMessage uses
    // for the return path.
    static std::array<std::uint8_t, 17>
    encodeMotionPayload(std::uint8_t controllerIndex, std::int16_t gyroX, std::int16_t gyroY,
                        std::int16_t gyroZ, std::int16_t accelX, std::int16_t accelY,
                        std::int16_t accelZ, std::uint32_t timestampDeltaUs);

    // Pure encoder for the MSG_BATTERY inner payload. Three bytes total:
    // ctrlIdx + level + status.
    static std::array<std::uint8_t, 3>
    encodeBatteryPayload(std::uint8_t controllerIndex, std::uint8_t level, std::uint8_t status);

    // Pure encoder for the MSG_TOUCHPAD inner payload. 12 bytes:
    // ctrlIdx(1) + flags(1) + finger0(id1 + x2 + y2) + finger1(id1 + x2 + y2).
    // `flags` bit 0 = finger0 active, bit 1 = finger1 active, bit 2 = button.
    // Coordinates are host-LE int16. Exposed statically so unit tests can pin
    // the byte layout without a live socket.
    static std::array<std::uint8_t, 12>
    encodeTouchpadPayload(std::uint8_t controllerIndex, bool finger0Active, std::uint8_t finger0Id,
                          std::int16_t finger0X, std::int16_t finger0Y, bool finger1Active,
                          std::uint8_t finger1Id, std::int16_t finger1X, std::int16_t finger1Y,
                          bool buttonPressed);

    // Pure encoder for the MSG_CONTROLLER_CAPS_UPDATE inner payload. Three
    // bytes total: ctrlIdx(1) + caps(2 BE). Mirrors the caps field of
    // MSG_CONTROLLER_ADD verbatim — same wire shape, different message type
    // — so a receiver shares the decode path. Exposed statically so the byte
    // layout can be pinned by unit tests without a live socket.
    static std::array<std::uint8_t, 3> encodeCapsUpdatePayload(std::uint8_t controllerIndex,
                                                               std::uint16_t capabilities);

    // Decoded MSG_CONTROLLER_ACK extension. Carries the legacy 4-byte payload
    // plus the optional motion-flags byte. `motionFlags == std::nullopt` is
    // the wire signal for "pre-extension satellite" (payload length 4); a
    // post-extension satellite always writes the byte (zero or not), and the
    // dish-side store collapses the nullopt onto "unknown" rather than
    // false-as-failure.
    struct ControllerAckMessage {
        std::uint16_t requestType = 0;
        std::uint8_t controllerIndex = 0;
        std::uint8_t result = 0;
        std::optional<std::uint8_t> motionFlags;
    };

    // Pure decoder for the MSG_CONTROLLER_ACK inner payload (the 4-byte
    // {type, length} header has already been stripped — `payload` points at
    // the post-header bytes and `len` is the number of payload bytes). The
    // satellite's authoritative encoder is sendControllerAck in
    // satellite/src/adapters/client_adapter.cpp.
    //
    // Wire layout:
    //   reqType(2 BE) ctrlIdx(1) result(1)            — legacy, len == 4
    //   reqType(2 BE) ctrlIdx(1) result(1) motion(1)  — extended, len >= 5
    // A truncated payload (< 4) returns std::nullopt — same shape as
    // parseRumbleMessage so the test seams match.
    static std::optional<ControllerAckMessage>
    parseControllerAckPayload(const std::uint8_t* payload, std::size_t len);

    // Decoded rumble message from the satellite — motors + duration only.
    struct RumbleMessage {
        int controllerIndex = 0;
        std::uint16_t strongMagnitude = 0;
        std::uint16_t weakMagnitude = 0;
        std::uint16_t durationMs = 0;
    };

    // Install (or replace) the rumble callback. Invoked from the receive
    // loop's thread for every parsed MSG_RUMBLE packet. The handler is
    // expected to enqueue / forward to the actuator without blocking; we
    // hold an internal lock around assignment to avoid a TOCTOU on the read
    // side, but the call itself runs unlocked.
    using RumbleHandler = std::function<void(const RumbleMessage&)>;
    void setRumbleHandler(RumbleHandler handler);

    // Length of the MSG_RUMBLE inner payload (after the 4-byte type+length
    // header), in bytes: ctrlIdx(1) + strong(2) + weak(2) + durMs(2).
    static constexpr std::size_t kRumblePayloadLen = 7;

    // Pure decoder for the MSG_RUMBLE inner payload (the 4-byte header
    // {type, length} has already been stripped). Returns std::nullopt on
    // truncation; see ClientAdapter::sendRumble for the producer side. Kept
    // public + static so it can be exercised by unit tests without a live
    // socket.
    //
    // Wire layout (fixed 7 bytes):
    //   ctrlIdx(1) strong(2 BE) weak(2 BE) durMs(2 BE)
    static std::optional<RumbleMessage> parseRumbleMessage(const std::uint8_t* payload,
                                                           std::size_t len);

    // Decoded lightbar message from the satellite (Task 1.4 dedicated stream).
    // Independent from MSG_RUMBLE so games that only change colour drive
    // the LED on the dish.
    struct LightbarMessage {
        int controllerIndex = 0;
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
    };

    using LightbarHandler = std::function<void(const LightbarMessage&)>;
    void setLightbarHandler(LightbarHandler handler);

    // Pure decoder for the MSG_LIGHTBAR inner payload. Wire layout:
    // ctrlIdx + r + g + b = 4 bytes exactly.
    static std::optional<LightbarMessage> parseLightbarMessage(const std::uint8_t* payload,
                                                               std::size_t len);

    void startHeartbeat();
    void stopHeartbeat();
    void startReceiveLoop();
    void stopReceiveLoop();

    bool isOpen() const { return sock_ >= 0; }
    bool isAlive() const { return connectionAlive_.load(std::memory_order_relaxed); }
    int missedAcks() const { return missedAcks_.load(std::memory_order_relaxed); }
    std::int32_t lastControllerAck() const {
        return lastControllerAck_.load(std::memory_order_relaxed);
    }
    // Latest MSG_CONTROLLER_ACK motion-flags byte for this session, or -1 if
    // no extended ACK has been observed (the satellite is pre-extension, or
    // no ACK at all has arrived). Bits as per kAckMotionFlag* — same shape
    // dish-android's getLastControllerMotionFlags returns. -1 is "unknown",
    // NOT "backend broken"; callers must collapse it onto an unknown state
    // rather than treating either bit as false, or an old satellite reads as
    // a permanent failure.
    std::int32_t lastControllerAckMotionFlags() const {
        return lastControllerAckMotionFlags_.load(std::memory_order_relaxed);
    }
    std::int8_t vigemAvailable() const { return vigemAvailable_.load(std::memory_order_relaxed); }
    std::int8_t activeControllerCount() const {
        return activeControllerCount_.load(std::memory_order_relaxed);
    }

  private:
    // Test-only seam: SatelliteClient with test-injected socket pair. Internal
    // function visible to friends; declared but never defined in production.
    friend class SatelliteClientTestAccess;

    void sendEncrypted(std::uint16_t msgType, const std::uint8_t* payload, std::size_t len);
    void heartbeatLoop();
    void receiveLoop();
    void processIncoming(const std::uint8_t* buf, std::size_t n);

    int sock_ = -1;
    sockaddr_in dest_{};
    std::array<std::uint8_t, 4> token_{};
    std::array<std::uint8_t, 32> key_{};
    util::AtomicCounter counter_;
    std::mutex sendLock_;

    std::atomic<bool> heartbeatRunning_{false};
    std::atomic<bool> ackRunning_{false};
    std::thread heartbeatThread_;
    std::thread ackThread_;

    std::atomic<int> missedAcks_{0};
    std::atomic<bool> connectionAlive_{true};
    std::atomic<std::int32_t> lastControllerAck_{-1};
    // Latest motion-flags byte from a post-extension MSG_CONTROLLER_ACK, or
    // -1 sentinel meaning "no extended ACK observed yet". Sentinel survives a
    // pre-extension satellite (which sends only the 4-byte legacy payload)
    // and is reset together with lastControllerAck_ on every reconnect /
    // re-registration — see resetControllerAck().
    std::atomic<std::int32_t> lastControllerAckMotionFlags_{-1};
    std::atomic<std::int8_t> vigemAvailable_{-1};
    std::atomic<std::int8_t> activeControllerCount_{-1};

    // Read on every parsed MSG_RUMBLE on the receive thread; written from
    // the owning thread (Qt main) via setRumbleHandler. A short critical
    // section (handler copy under lock) keeps the hot-path call unlocked.
    std::mutex rumbleHandlerMtx_;
    RumbleHandler rumbleHandler_;

    // Same shape but for MSG_LIGHTBAR (Task 1.4 dedicated stream).
    std::mutex lightbarHandlerMtx_;
    LightbarHandler lightbarHandler_;
};

} // namespace dish::net
