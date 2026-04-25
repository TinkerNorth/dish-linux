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
#include <mutex>
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
    void resetControllerAck() { lastControllerAck_.store(-1, std::memory_order_relaxed); }

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
    std::int8_t vigemAvailable() const {
        return vigemAvailable_.load(std::memory_order_relaxed);
    }
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
    std::atomic<std::int8_t> vigemAvailable_{-1};
    std::atomic<std::int8_t> activeControllerCount_{-1};
};

}  // namespace dish::net
