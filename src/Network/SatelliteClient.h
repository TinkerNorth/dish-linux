// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Encrypted UDP data-plane session to a single Satellite server (protocol-1).
// The Linux analogue of dish-windows/Network/SatelliteClient and dish-android's
// satellite_jni UDP path. Owns one raw POSIX UDP socket, the per-session
// ChaCha20-Poly1305 key/token, a monotonic per-direction nonce counter
// (starting at 1), a heartbeat sender thread and a receive thread.
//
// Topology is REST-only now: this class carries STREAMS only (input, heartbeat,
// motion, battery, touchpad up; heartbeat-ack, rumble, lightbar, session-close
// down). The deleted opcodes 0x0004/5/6/7/8/E and the UDP controller-
// registration polling are gone.
//
// Crypto is delegated to Network/SessionCrypto (frozen, byte-exact vs the
// satellite): nonce = dir(1)|0x00×7|counter(4 BE), AAD = token(4 BE), key = the
// HKDF-derived sessionKey (NEVER the raw pairing key). See contract §Crypto.
//
// Thread-safety:
//   * sendReport() is called directly from the SDL gamepad thread for minimum
//     latency; it holds materialMtx_ only to draw (key, token, counter)
//     together and sendLock_ only for the one ::sendto.
//   * Public lifecycle calls (open/close, setConnectionParams, start/stop) are
//     expected to be invoked from a single owner thread (typically the Qt main
//     thread). setConnectionParams MAY run against a live session (proactive
//     re-key): materialMtx_ makes the material swap atomic vs both loops.

#pragma once

#include "Models/Protocol.h"
#include "Util/AtomicCounter.h"
#include "Util/LatencyWindow.h"

#include <netinet/in.h>

#include <algorithm>
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
    // Opcodes mirror Models/Protocol.h (the single Qt-free source). Kept as
    // members so existing call sites and tests can reference them by class.
    // The topology opcodes 0x0004-0x0008 and 0x000E are deleted in protocol-1
    // and intentionally have no constant here — a stray reference fails to
    // compile.
    static constexpr std::uint16_t kMsgGamepadData = proto::kMsgInput;
    static constexpr std::uint16_t kMsgHeartbeatPing = proto::kMsgHeartbeat;
    static constexpr std::uint16_t kMsgHeartbeatAck = proto::kMsgHeartbeatAck;
    static constexpr std::uint16_t kMsgRumble = proto::kMsgRumble;
    static constexpr std::uint16_t kMsgMotion = proto::kMsgMotion;
    static constexpr std::uint16_t kMsgBattery = proto::kMsgBattery;
    static constexpr std::uint16_t kMsgTouchpad = proto::kMsgTouchpad;
    static constexpr std::uint16_t kMsgLightbar = proto::kMsgLightbar;
    static constexpr std::uint16_t kMsgSessionClose = proto::kMsgSessionClose;

    // Controller capability bits (carried in the REST descriptor caps object;
    // kept here too so SDL-bound capability folding stays unit-testable).
    static constexpr std::uint16_t kCapAnalogTriggers = proto::kCapAnalogTriggers;
    static constexpr std::uint16_t kCapRumble = proto::kCapRumble;
    static constexpr std::uint16_t kCapMotion = proto::kCapMotion;
    static constexpr std::uint16_t kCapLightbar = proto::kCapLightbar;

    // Battery wire constants (satellite/src/core/types.h mirrors).
    static constexpr std::uint8_t kBatteryLevelUnknown = 0xFF;
    static constexpr std::uint8_t kBatteryStatusUnknown = 0;
    static constexpr std::uint8_t kBatteryStatusDischarging = 1;
    static constexpr std::uint8_t kBatteryStatusCharging = 2;
    static constexpr std::uint8_t kBatteryStatusFull = 3;
    static constexpr std::uint8_t kBatteryStatusWired = 4;

    // Heartbeat cadence is 2000 ms; "not responding" at 2 misses, dead at 5
    // (contract §Liveness).
    static constexpr std::uint32_t kHeartbeatIntervalMs = 2000;
    static constexpr int kHeartbeatMissNotResponding = 2;
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

    // Install the post-PUT token (4B) + the HKDF-derived 32B sessionKey (see
    // wire::deriveSessionKey — NEVER the raw pairing key). Resets the send
    // counter to 1, the recv replay guard, the enriched-ack mirrors, the
    // close-reason latch and the latency window. Call after each session PUT
    // (token/salt/key rotate; counters restart at 1 with no nonce reuse).
    void setConnectionParams(const std::array<std::uint8_t, 4>& token,
                             const std::array<std::uint8_t, 32>& sessionKey);

    // Hot path: called directly from the SDL input thread.
    void sendReport(int controllerIndex, std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt,
                    std::int16_t lx, std::int16_t ly, std::int16_t rx, std::int16_t ry);

    // Pure helper: fold the per-controller CAP_LIGHTBAR (0x0008) bit into a
    // base capability word. Returns `base` unchanged when the bound pad has
    // no addressable RGB LED, and `base | kCapLightbar` when it does. Kept
    // public + static so the cap-advertisement rule is unit-testable without
    // a live socket — the word now rides the REST descriptor caps object.
    static std::uint16_t withLightbarCapability(std::uint16_t base, bool hasLightbar) {
        return static_cast<std::uint16_t>(base | (hasLightbar ? kCapLightbar : 0));
    }

    // Pure helper: fold the per-controller CAP_MOTION (0x0004) bit into a
    // base capability word — an Xbox pad therefore never advertises CAP_MOTION.
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
    // orientation. The contract's "senders apply the rotation" rule is
    // satisfied by SDL on our behalf — do NOT add a rotation step here or it
    // would double-apply.
    //
    // Scale: gyro int16 LSB = 2000/32767 deg/s; accel int16 LSB = 4/32767 g.
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
    // `buttonPressed` is the clickable-pad switch. `eventTimeMs` is the
    // sender-side sample timestamp (uptime ms, u32 — SDL's event timestamp) —
    // protocol-1 requires it (mouse-mode timing depends on it).
    //
    // Hot path: called from the SDL touchpad-event thread.
    void sendTouchpad(int controllerIndex, bool finger0Active, std::uint8_t finger0Id,
                      std::int16_t finger0X, std::int16_t finger0Y, bool finger1Active,
                      std::uint8_t finger1Id, std::int16_t finger1X, std::int16_t finger1Y,
                      bool buttonPressed, std::uint32_t eventTimeMs);

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

    // Pure encoder for the MSG_TOUCHPAD inner payload. 16 bytes:
    // ctrlIdx(1) + flags(1) + f0(id1 + x2 + y2) + f1(id1 + x2 + y2) +
    // eventTimeMs(u32 LE). `flags` bit 0 = finger0 active, bit 1 = finger1
    // active, bit 2 = button. Coordinates + eventTimeMs are host-LE. The
    // trailing eventTimeMs is the protocol-1 addition (was 12 bytes total);
    // the server now requires msgLen >= 16 inner so a 12-byte body is dropped.
    static std::array<std::uint8_t, 16>
    encodeTouchpadPayload(std::uint8_t controllerIndex, bool finger0Active, std::uint8_t finger0Id,
                          std::int16_t finger0X, std::int16_t finger0Y, bool finger1Active,
                          std::uint8_t finger1Id, std::int16_t finger1X, std::int16_t finger1Y,
                          bool buttonPressed, std::uint32_t eventTimeMs);

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
    // truncation. Kept public + static so it can be exercised by unit tests
    // without a live socket.
    //
    // Wire layout (fixed 7 bytes):
    //   ctrlIdx(1) strong(2 BE) weak(2 BE) durMs(2 BE)
    static std::optional<RumbleMessage> parseRumbleMessage(const std::uint8_t* payload,
                                                           std::size_t len);

    // Decoded lightbar message from the satellite (dedicated colour stream).
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

    // Decoded enriched heartbeat ack (MSG_HEARTBEAT_ACK 0x0003):
    // backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) +
    // activeBitmap(u16 BE). Drives the reconcile loop.
    struct HeartbeatAck {
        bool backendAvailable = false;
        std::uint8_t totalActiveControllers = 0;
        std::uint16_t epoch = 0;
        std::uint16_t activeBitmap = 0;
    };

    // Pure decoder for the MSG_HEARTBEAT_ACK inner payload (header stripped).
    // std::nullopt when shorter than kHeartbeatAckPayloadBytes (a bare ack from
    // a pre-protocol-1 server is then ignored for reconcile, only liveness
    // counts). Public + static so the parse is unit-testable.
    static std::optional<HeartbeatAck> parseHeartbeatAck(const std::uint8_t* payload,
                                                         std::size_t len);

    void startHeartbeat();
    void stopHeartbeat();
    void startReceiveLoop();
    void stopReceiveLoop();

    bool isOpen() const { return sock_ >= 0; }
    bool isAlive() const { return connectionAlive_.load(std::memory_order_relaxed); }
    int missedAcks() const { return missedAcks_.load(std::memory_order_relaxed); }
    // Last enriched-ack values, surfaced for the reconcile poll (the session
    // polls these atomics from its main-thread alive tick). -1 until the first
    // enriched ack arrives; reset by setConnectionParams.
    std::int32_t serverEpoch() const { return serverEpoch_.load(std::memory_order_relaxed); }
    std::int32_t serverBitmap() const { return serverBitmap_.load(std::memory_order_relaxed); }
    std::int8_t backendAvailable() const {
        return backendAvailable_.load(std::memory_order_relaxed);
    }
    std::int8_t activeControllerCount() const {
        return activeControllerCount_.load(std::memory_order_relaxed);
    }
    // The reason byte of a received SESSION_CLOSE (0x000F), or -1 if none. The
    // session's alive-poll treats a non-negative value as terminal-now (no
    // death wait). Latched until the next setConnectionParams.
    std::int32_t sessionCloseReason() const {
        return sessionCloseReason_.load(std::memory_order_relaxed);
    }
    // Current send counter (next value to use), for the proactive-re-PUT guard
    // (reducer::counterNeedsRepush). Clamped, not truncated: past exhaustion
    // the poll must keep reading re-PUT needed, never wrap under the threshold.
    std::uint32_t sendCounter() const {
        return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(sendCounter_.load(), 0xFFFFFFFFu));
    }

    // One-way latency estimate off the heartbeat round trip: median of the
    // sliding RTT window halved + the sample count the UI shows beside the
    // figure (Util/LatencyWindow). Zeroed by setConnectionParams (the readout
    // answers THIS session). Poll from the main-thread alive tick; samples
    // land on the receive thread under the same lock.
    struct LatencySnapshot {
        double oneWayMs = 0.0;
        int samples = 0;
    };
    LatencySnapshot latencySnapshot() const;

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
    // Session material + both counters live behind materialMtx_: a proactive
    // re-key swaps them on a LIVE session, and pairing an old key with a fresh
    // counter (or a stale replay mark with a fresh session) breaks the nonce
    // invariant. Senders draw (key, token, counter) in one hold and encrypt on
    // the copies; the receive loop snapshots the same way.
    mutable std::mutex materialMtx_;
    std::array<std::uint8_t, 4> token_{};
    std::uint32_t tokenBe_ = 0; // token as a host u32 (the 4 raw BE bytes), for the AAD
    std::array<std::uint8_t, 32> key_{};
    // Per-direction send counter, starting at 1 (contract §Crypto). 64-bit so
    // exhaustion parks the sender silent instead of wrapping the 32-bit wire
    // field into nonce reuse. Atomic for the lock-free sendCounter() poll;
    // draws pair with the key under materialMtx_.
    util::AtomicCounter sendCounter_{1};
    // Receiver replay guard (server→client direction): drop counter <=
    // lastRecvCounter_ (first packet exempt while it is 0). Advanced only by
    // the receive thread; reset under materialMtx_ by a re-key.
    std::uint32_t lastRecvCounter_ = 0;
    std::mutex sendLock_;

    std::atomic<bool> heartbeatRunning_{false};
    std::atomic<bool> ackRunning_{false};
    std::thread heartbeatThread_;
    std::thread ackThread_;

    // Monotonic stamp (µs) of the heartbeat ping currently in flight; 0 = none.
    // Armed by the heartbeat thread (per reducer::shouldArmPing — never
    // overwritten while a ping is outstanding: a late ack paired with a newer
    // ping's stamp would read artificially low), consumed by the receive
    // thread's ack handler via exchange(0). Lives on the client (one per
    // session) so concurrent satellites never cross-pair a ping with another's
    // ack. dish-android #138 design.
    std::atomic<std::int64_t> pingSentUs_{0};
    // The sliding RTT window behind latencySnapshot(). The receive thread
    // pushes; the main-thread poll reads — both under latencyMtx_.
    mutable std::mutex latencyMtx_;
    reducer::LatencyWindow latencyWindow_;

    std::atomic<int> missedAcks_{0};
    std::atomic<bool> connectionAlive_{true};
    std::atomic<std::int32_t> serverEpoch_{-1};
    std::atomic<std::int32_t> serverBitmap_{-1};
    std::atomic<std::int8_t> backendAvailable_{-1};
    std::atomic<std::int8_t> activeControllerCount_{-1};
    std::atomic<std::int32_t> sessionCloseReason_{-1};

    // Read on every parsed MSG_RUMBLE on the receive thread; written from
    // the owning thread (Qt main) via setRumbleHandler. A short critical
    // section (handler copy under lock) keeps the hot-path call unlocked.
    std::mutex rumbleHandlerMtx_;
    RumbleHandler rumbleHandler_;

    // Same shape but for MSG_LIGHTBAR (dedicated colour stream).
    std::mutex lightbarHandlerMtx_;
    LightbarHandler lightbarHandler_;
};

} // namespace dish::net
