// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Encrypted UDP data-plane session to one satellite: a raw Winsock socket, the
// session key/token, and the heartbeat and receive threads. Streams only, since
// topology moved to REST. Crypto is delegated to core/wire/SessionCrypto, which
// is frozen byte-exact against the satellite.

#pragma once

#include "Util/AtomicCounter.h"
#include "core/model/Protocol.h"
#include "core/reducer/LatencyWindow.h"
#include "core/reducer/Reconcile.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

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

// sendReport() runs on the SDL input thread. Every other public call is expected
// on one owner thread (Qt main), but setConnectionParams MAY land on a live
// session for a proactive re-key, so materialMtx_ makes the swap atomic against
// both loops.
class SatelliteClient {
  public:
    // Aliases of core/model/Protocol.h, the single Qt-free source.
    static constexpr std::uint16_t kMsgInput = proto::kMsgInput;
    static constexpr std::uint16_t kMsgHeartbeatPing = proto::kMsgHeartbeat;
    static constexpr std::uint16_t kMsgHeartbeatAck = proto::kMsgHeartbeatAck;
    static constexpr std::uint16_t kMsgRumble = proto::kMsgRumble;
    static constexpr std::uint16_t kMsgMotion = proto::kMsgMotion;
    static constexpr std::uint16_t kMsgBattery = proto::kMsgBattery;
    static constexpr std::uint16_t kMsgTouchpad = proto::kMsgTouchpad;
    static constexpr std::uint16_t kMsgLightbar = proto::kMsgLightbar;
    static constexpr std::uint16_t kMsgSessionClose = proto::kMsgSessionClose;

    // Carried in the REST descriptor's caps object.
    static constexpr std::uint16_t kCapAnalogTriggers = proto::kCapAnalogTriggers;
    static constexpr std::uint16_t kCapRumble = proto::kCapRumble;
    static constexpr std::uint16_t kCapMotion = proto::kCapMotion;
    static constexpr std::uint16_t kCapLightbar = proto::kCapLightbar;

    // Wire values, mirroring satellite/src/core/types.h.
    static constexpr std::uint8_t kBatteryLevelUnknown = 0xFF;
    static constexpr std::uint8_t kBatteryStatusUnknown = 0;
    static constexpr std::uint8_t kBatteryStatusDischarging = 1;
    static constexpr std::uint8_t kBatteryStatusCharging = 2;
    static constexpr std::uint8_t kBatteryStatusFull = 3;
    static constexpr std::uint8_t kBatteryStatusWired = 4;

    // Pinned to satellite types.h HEARTBEAT_INTERVAL_SEC / HEARTBEAT_MISS_MAX.
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

    // Takes the HKDF-derived session key, never the raw pairing key. Must be
    // called after every session PUT: the token and salt rotate there, and this
    // restarts both counters at 1, which is only safe against a fresh key.
    void setConnectionParams(const std::array<std::uint8_t, 4>& token,
                             const std::array<std::uint8_t, 32>& key);

    // Hot path: called directly from the SDL input thread.
    void sendReport(int controllerIndex, std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt,
                    std::int16_t lx, std::int16_t ly, std::int16_t rx, std::int16_t ry);

    static std::uint16_t withLightbarCapability(std::uint16_t base, bool hasLightbar) {
        return static_cast<std::uint16_t>(base | (hasLightbar ? kCapLightbar : 0));
    }

    static std::uint16_t withMotionCapability(std::uint16_t base, bool hasMotion) {
        return static_cast<std::uint16_t>(base | (hasMotion ? kCapMotion : 0));
    }

    static std::uint16_t withRumbleCapability(std::uint16_t base, bool hasRumble) {
        return static_cast<std::uint16_t>(base | (hasRumble ? kCapRumble : 0));
    }

    // Axes are the satellite's right-handed frame (+X right, +Y up, +Z toward
    // player); SDL already applies the manufacturer rotation for HIDAPI pads, so
    // samples arrive in that frame. Scale: gyro LSB = 2000/32767 deg/s, accel LSB
    // = 4/32767 g. `timestampDeltaUs` is 0 on the controller's first packet.
    void sendMotion(int controllerIndex, std::int16_t gyroX, std::int16_t gyroY, std::int16_t gyroZ,
                    std::int16_t accelX, std::int16_t accelY, std::int16_t accelZ,
                    std::uint32_t timestampDeltaUs);

    // `level` is 0..100 or kBatteryLevelUnknown; `status` a kBatteryStatus*.
    void sendBattery(int controllerIndex, std::uint8_t level, std::uint8_t status);

    // `eventTimeMs` is the sender-side uptime stamp; the host's mouse-mode timing
    // depends on it, so it is not optional. Coordinates are normalised int16.
    void sendTouchpad(int controllerIndex, bool finger0Active, std::uint8_t finger0Id,
                      std::int16_t finger0X, std::int16_t finger0Y, bool finger1Active,
                      std::uint8_t finger1Id, std::int16_t finger1X, std::int16_t finger1Y,
                      bool buttonPressed, std::uint32_t eventTimeMs);

    // MSG_MOTION inner payload, after the 4-byte type+length header: ctrlIdx +
    // 6×i16 + u32, little-endian to match the satellite's decodeMotionReport.
    static std::array<std::uint8_t, 17>
    encodeMotionPayload(std::uint8_t controllerIndex, std::int16_t gyroX, std::int16_t gyroY,
                        std::int16_t gyroZ, std::int16_t accelX, std::int16_t accelY,
                        std::int16_t accelZ, std::uint32_t timestampDeltaUs);

    // ctrlIdx + level + status.
    static std::array<std::uint8_t, 3>
    encodeBatteryPayload(std::uint8_t controllerIndex, std::uint8_t level, std::uint8_t status);

    // ctrlIdx(1) + flags(1) + f0(id1 x2 y2) + f1(id1 x2 y2) + eventTimeMs(u32),
    // little-endian. flags bit0 = finger0 active, bit1 = finger1, bit2 = button.
    // The 15 bytes after ctrlIdx match satellite types.h::TouchpadReport.
    static std::array<std::uint8_t, 16>
    encodeTouchpadPayload(std::uint8_t controllerIndex, bool finger0Active, std::uint8_t finger0Id,
                          std::int16_t finger0X, std::int16_t finger0Y, bool finger1Active,
                          std::uint8_t finger1Id, std::int16_t finger1X, std::int16_t finger1Y,
                          bool buttonPressed, std::uint32_t eventTimeMs);

    struct RumbleMessage {
        int controllerIndex = 0;
        std::uint16_t strongMagnitude = 0;
        std::uint16_t weakMagnitude = 0;
        std::uint16_t durationMs = 0;
    };

    using RumbleHandler = std::function<void(const RumbleMessage&)>;
    void setRumbleHandler(RumbleHandler handler);

    static constexpr std::size_t kRumblePayloadLen = 7;

    // Header already stripped. Fixed 7 bytes: ctrlIdx(1) strong(2) weak(2)
    // durMs(2), big-endian unlike the LE up-stream payloads.
    static std::optional<RumbleMessage> parseRumbleMessage(const std::uint8_t* payload,
                                                           std::size_t len);

    struct LightbarMessage {
        int controllerIndex = 0;
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
    };

    using LightbarHandler = std::function<void(const LightbarMessage&)>;
    void setLightbarHandler(LightbarHandler handler);

    // ctrlIdx + r + g + b.
    static std::optional<LightbarMessage> parseLightbarMessage(const std::uint8_t* payload,
                                                               std::size_t len);

    // backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) +
    // activeBitmap(u16 BE). Drives the reconcile loop.
    struct HeartbeatAck {
        bool backendAvailable = false;
        std::uint8_t totalActiveControllers = 0;
        std::uint16_t epoch = 0;
        std::uint16_t activeBitmap = 0;
    };

    // nullopt for a short payload, which is the bare ack an older server sends:
    // it still counts for liveness but carries nothing to reconcile against.
    static std::optional<HeartbeatAck> parseHeartbeatAck(const std::uint8_t* payload,
                                                         std::size_t len);

    // Invoked on the receive thread.
    using HeartbeatAckHandler = std::function<void(const HeartbeatAck&)>;
    void setHeartbeatAckHandler(HeartbeatAckHandler handler);

    // Invoked on the receive thread with the proto::kCloseReason* byte, which the
    // session maps to a teardown action.
    using CloseHandler = std::function<void(std::uint8_t reason)>;
    void setCloseHandler(CloseHandler handler);

    void startHeartbeat();
    void stopHeartbeat();
    void startReceiveLoop();
    void stopReceiveLoop();

    bool isOpen() const { return sock_ >= 0; }
    bool isAlive() const { return connectionAlive_.load(std::memory_order_relaxed); }
    int missedAcks() const { return missedAcks_.load(std::memory_order_relaxed); }
    // Poll alternative to the ack handler. -1 until the first enriched ack.
    std::int32_t serverEpoch() const { return serverEpoch_.load(std::memory_order_relaxed); }
    std::int32_t serverBitmap() const { return serverBitmap_.load(std::memory_order_relaxed); }
    std::int8_t backendAvailable() const {
        return backendAvailable_.load(std::memory_order_relaxed);
    }
    std::int8_t activeControllerCount() const {
        return activeControllerCount_.load(std::memory_order_relaxed);
    }
    // -1 if no SESSION_CLOSE arrived. Non-negative is terminal immediately, so
    // the alive-poll need not wait out the heartbeat death count.
    std::int32_t sessionCloseReason() const {
        return sessionCloseReason_.load(std::memory_order_relaxed);
    }
    // The next counter value, for the proactive re-PUT guard. Clamped rather than
    // truncated so that past exhaustion the poll keeps reading "re-PUT needed"
    // instead of wrapping back under the threshold.
    std::uint32_t sendCounter() const { return reducer::clampedSendCounter(sendCounter_.load()); }

    // Half the median of the sliding heartbeat-RTT window. Zeroed by
    // setConnectionParams so the readout always answers the current session.
    // Polled from the main thread; samples land on the receive thread.
    struct LatencySnapshot {
        double oneWayMs = 0.0;
        int samples = 0;
    };
    LatencySnapshot latencySnapshot() const;

  private:
    // Test-only seam to park the send counter near exhaustion.
    friend class SatelliteClientTestAccess;

    void sendEncrypted(std::uint16_t msgType, const std::uint8_t* payload, std::size_t len);
    void heartbeatLoop();
    void receiveLoop();
    void processIncoming(const std::uint8_t* buf, std::size_t n);

    int sock_ = -1;
    sockaddr_in dest_{};
    // Material and both counters share materialMtx_ because a re-key swaps them
    // on a LIVE session: pairing an old key with a fresh counter would reuse a
    // nonce. Senders draw (key, token, counter) in one hold and encrypt on the
    // copies.
    mutable std::mutex materialMtx_;
    std::array<std::uint8_t, 4> token_{};
    std::uint32_t tokenBe_ = 0; // the 4 raw BE bytes as a host u32, for the AAD
    std::array<std::uint8_t, 32> key_{};
    // 64-bit so exhaustion parks the sender silent rather than wrapping the
    // 32-bit wire field into nonce reuse. Atomic for the lock-free poll.
    util::AtomicCounter sendCounter_{1};
    // Replay guard: drop counter <= this, with the first packet exempt while it
    // is 0. Advanced only by the receive thread, reset by a re-key.
    std::uint32_t lastRecvCounter_ = 0;
    std::mutex sendLock_;

    std::atomic<bool> heartbeatRunning_{false};
    std::atomic<bool> ackRunning_{false};
    std::thread heartbeatThread_;
    std::thread ackThread_;

    // µs stamp of the in-flight heartbeat ping, 0 for none. Armed on the
    // heartbeat thread, consumed on the receive thread via exchange(0). Per
    // client, so concurrent satellites cannot cross-pair a ping with another's
    // ack.
    std::atomic<std::int64_t> pingSentUs_{0};
    mutable std::mutex latencyMtx_;
    reducer::LatencyWindow latencyWindow_;

    std::atomic<int> missedAcks_{0};
    std::atomic<bool> connectionAlive_{true};
    std::atomic<std::int32_t> serverEpoch_{-1};
    std::atomic<std::int32_t> serverBitmap_{-1};
    std::atomic<std::int8_t> backendAvailable_{-1};
    std::atomic<std::int8_t> activeControllerCount_{-1};
    std::atomic<std::int32_t> sessionCloseReason_{-1};

    std::mutex rumbleHandlerMtx_;
    RumbleHandler rumbleHandler_;
    std::mutex lightbarHandlerMtx_;
    LightbarHandler lightbarHandler_;
    std::mutex ackHandlerMtx_;
    HeartbeatAckHandler ackHandler_;
    std::mutex closeHandlerMtx_;
    CloseHandler closeHandler_;
};

} // namespace dish::net
