// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SatelliteClient.h"

#include "Network/SessionCrypto.h"
#include "Util/Endian.h"

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <sodium.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace dish::net {

using util::putU16Be;
using util::putU32Be;
using util::readU16Be;

namespace {
// 16-byte Poly1305 tag appended by the AEAD.
constexpr std::size_t kAuthTag = 16;
constexpr std::size_t kHeaderSize = 8; // token(4) + counter(4)

// Monotonic microseconds for the heartbeat ping clock. steady_clock never runs
// backwards; 0 is reserved as the "no ping in flight" sentinel.
std::int64_t nowSteadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

SatelliteClient::SatelliteClient() {
    if (sodium_init() < 0) {
        // sodium_init is idempotent and returns 1 if already initialised; <0 is fatal.
    }
}

SatelliteClient::~SatelliteClient() { closeSocket(); }

bool SatelliteClient::openSocket(const std::string& ip, int port) {
    const int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { return false; }

    // DSCP EF (Expedited Forwarding). Best-effort — many Wi-Fi stacks silently
    // strip TOS but never error, matching the Android JNI / Mac client.
    int tos = 0xB8;
    ::setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    // 500 ms recv timeout so the receive loop can poll `ackRunning_` cleanly.
    timeval rtv{};
    rtv.tv_sec = 0;
    rtv.tv_usec = 500'000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(s);
        return false;
    }

    sock_ = s;
    dest_ = addr;
    return true;
}

void SatelliteClient::closeSocket() {
    stopHeartbeat();
    stopReceiveLoop();
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

void SatelliteClient::setConnectionParams(const std::array<std::uint8_t, 4>& token,
                                          const std::array<std::uint8_t, 32>& sessionKey) {
    token_ = token;
    tokenBe_ = util::readU32Be(token.data()); // the 4 raw token bytes are already big-endian
    key_ = sessionKey;
    // Counters restart at 1 every PUT (no cross-session nonce reuse); recv
    // guard resets so the first server packet is accepted.
    sendCounter_.store(1, std::memory_order_relaxed);
    lastRecvCounter_ = 0;
    missedAcks_.store(0, std::memory_order_relaxed);
    connectionAlive_.store(true, std::memory_order_relaxed);
    serverEpoch_.store(-1, std::memory_order_relaxed);
    serverBitmap_.store(-1, std::memory_order_relaxed);
    backendAvailable_.store(-1, std::memory_order_relaxed);
    activeControllerCount_.store(-1, std::memory_order_relaxed);
    sessionCloseReason_.store(-1, std::memory_order_relaxed);
    // The ping clock + RTT window restart with the session: a stale in-flight
    // stamp must not pair with the new session's first ack, and the readout
    // must answer this session only.
    pingSentUs_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(latencyMtx_);
        latencyWindow_.reset();
    }
}

void SatelliteClient::sendReport(int controllerIndex, std::uint16_t buttons, std::uint8_t lt,
                                 std::uint8_t rt, std::int16_t lx, std::int16_t ly, std::int16_t rx,
                                 std::int16_t ry) {
    // Payload: controllerIndex(1) + XUSB_REPORT(12 LE) = 13 bytes.
    std::uint8_t payload[13]{};
    payload[0] = static_cast<std::uint8_t>(controllerIndex);
    payload[1] = static_cast<std::uint8_t>(buttons & 0xFFU);
    payload[2] = static_cast<std::uint8_t>((buttons >> 8) & 0xFFU);
    payload[3] = lt;
    payload[4] = rt;
    auto storeLe16 = [&](int off, std::int16_t v) {
        const auto u = static_cast<std::uint16_t>(v);
        payload[off] = static_cast<std::uint8_t>(u & 0xFFU);
        payload[off + 1] = static_cast<std::uint8_t>((u >> 8) & 0xFFU);
    };
    storeLe16(5, lx);
    storeLe16(7, ly);
    storeLe16(9, rx);
    storeLe16(11, ry);
    sendEncrypted(kMsgGamepadData, payload, sizeof(payload));
}

std::array<std::uint8_t, 17> SatelliteClient::encodeMotionPayload(
    std::uint8_t controllerIndex, std::int16_t gyroX, std::int16_t gyroY, std::int16_t gyroZ,
    std::int16_t accelX, std::int16_t accelY, std::int16_t accelZ, std::uint32_t timestampDeltaUs) {
    // ctrlIdx(1) + 6×int16 LE + uint32 LE = 1 + 12 + 4 = 17 bytes.
    // Matches MotionReport's little-endian wire layout. The receiver does NOT
    // memcpy onto the struct — it decodes via explicit byte-shifts
    // (decodeMotionReport in satellite/src/core/types.h), so the contract is
    // byte-order- and struct-layout-independent. Senders therefore write LE
    // explicitly here.
    std::array<std::uint8_t, 17> out{};
    out[0] = controllerIndex;
    auto storeLe16 = [&out](std::size_t off, std::int16_t v) {
        const auto u = static_cast<std::uint16_t>(v);
        out[off] = static_cast<std::uint8_t>(u & 0xFFU);
        out[off + 1] = static_cast<std::uint8_t>((u >> 8) & 0xFFU);
    };
    auto storeLe32 = [&out](std::size_t off, std::uint32_t v) {
        out[off] = static_cast<std::uint8_t>(v & 0xFFU);
        out[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
        out[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
        out[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
    };
    storeLe16(1, gyroX);
    storeLe16(3, gyroY);
    storeLe16(5, gyroZ);
    storeLe16(7, accelX);
    storeLe16(9, accelY);
    storeLe16(11, accelZ);
    storeLe32(13, timestampDeltaUs);
    return out;
}

void SatelliteClient::sendMotion(int controllerIndex, std::int16_t gyroX, std::int16_t gyroY,
                                 std::int16_t gyroZ, std::int16_t accelX, std::int16_t accelY,
                                 std::int16_t accelZ, std::uint32_t timestampDeltaUs) {
    const auto payload =
        encodeMotionPayload(static_cast<std::uint8_t>(controllerIndex), gyroX, gyroY, gyroZ, accelX,
                            accelY, accelZ, timestampDeltaUs);
    sendEncrypted(kMsgMotion, payload.data(), payload.size());
}

std::array<std::uint8_t, 3> SatelliteClient::encodeBatteryPayload(std::uint8_t controllerIndex,
                                                                  std::uint8_t level,
                                                                  std::uint8_t status) {
    return {controllerIndex, level, status};
}

void SatelliteClient::sendBattery(int controllerIndex, std::uint8_t level, std::uint8_t status) {
    const auto payload =
        encodeBatteryPayload(static_cast<std::uint8_t>(controllerIndex), level, status);
    sendEncrypted(kMsgBattery, payload.data(), payload.size());
}

std::array<std::uint8_t, 16> SatelliteClient::encodeTouchpadPayload(
    std::uint8_t controllerIndex, bool finger0Active, std::uint8_t finger0Id, std::int16_t finger0X,
    std::int16_t finger0Y, bool finger1Active, std::uint8_t finger1Id, std::int16_t finger1X,
    std::int16_t finger1Y, bool buttonPressed, std::uint32_t eventTimeMs) {
    // ctrlIdx(1) + flags(1) + f0(id1 + x2 + y2) + f1(id1 + x2 + y2) +
    // eventTimeMs(u32 LE) = 16 bytes. The trailing eventTimeMs is the
    // protocol-1 addition — the server now requires the 15-byte post-ctrlIdx
    // body (msgLen >= 16 inner) or it drops the packet.
    std::array<std::uint8_t, 16> out{};
    out[0] = controllerIndex;
    std::uint8_t flags = 0;
    if (finger0Active) { flags |= 0x01U; }
    if (finger1Active) { flags |= 0x02U; }
    if (buttonPressed) { flags |= 0x04U; }
    out[1] = flags;
    auto storeLe16 = [&out](std::size_t off, std::int16_t v) {
        const auto u = static_cast<std::uint16_t>(v);
        out[off] = static_cast<std::uint8_t>(u & 0xFFU);
        out[off + 1] = static_cast<std::uint8_t>((u >> 8) & 0xFFU);
    };
    out[2] = finger0Id;
    storeLe16(3, finger0X);
    storeLe16(5, finger0Y);
    out[7] = finger1Id;
    storeLe16(8, finger1X);
    storeLe16(10, finger1Y);
    out[12] = static_cast<std::uint8_t>(eventTimeMs & 0xFFU);
    out[13] = static_cast<std::uint8_t>((eventTimeMs >> 8) & 0xFFU);
    out[14] = static_cast<std::uint8_t>((eventTimeMs >> 16) & 0xFFU);
    out[15] = static_cast<std::uint8_t>((eventTimeMs >> 24) & 0xFFU);
    return out;
}

void SatelliteClient::sendTouchpad(int controllerIndex, bool finger0Active, std::uint8_t finger0Id,
                                   std::int16_t finger0X, std::int16_t finger0Y, bool finger1Active,
                                   std::uint8_t finger1Id, std::int16_t finger1X,
                                   std::int16_t finger1Y, bool buttonPressed,
                                   std::uint32_t eventTimeMs) {
    const auto payload = encodeTouchpadPayload(
        static_cast<std::uint8_t>(controllerIndex), finger0Active, finger0Id, finger0X, finger0Y,
        finger1Active, finger1Id, finger1X, finger1Y, buttonPressed, eventTimeMs);
    sendEncrypted(kMsgTouchpad, payload.data(), payload.size());
}

void SatelliteClient::sendEncrypted(std::uint16_t msgType, const std::uint8_t* payload,
                                    std::size_t len) {
    if (sock_ < 0) { return; }
    // Inner: msgType(BE16) + payloadLen(BE16) + payload.
    const std::size_t innerLen = 4 + len;
    std::vector<std::uint8_t> inner(innerLen);
    putU16Be(inner.data(), msgType);
    putU16Be(inner.data() + 2, static_cast<std::uint16_t>(len));
    if (len > 0) { std::memcpy(inner.data() + 4, payload, len); }

    // Monotonic per-direction counter, starting at 1; never wraps (the session
    // self-heals via re-PUT before exhaustion — see WifiConnectionManager).
    const std::uint32_t ctr = sendCounter_.fetch_add(1, std::memory_order_relaxed);

    // Packet: token(4) | counter(4 BE) | ciphertext+tag. The AEAD nonce
    // (dir|0×7|counter) and AAD (token BE) are built inside wire::encryptPacket.
    std::vector<std::uint8_t> packet(kHeaderSize + innerLen + kAuthTag);
    std::memcpy(packet.data(), token_.data(), 4);
    putU32Be(packet.data() + 4, ctr);

    unsigned long long cipherLen = 0;
    if (!wire::encryptPacket(key_.data(), wire::kDirClientToServer, ctr, tokenBe_, inner.data(),
                             inner.size(), packet.data() + kHeaderSize, &cipherLen)) {
        return;
    }
    packet.resize(kHeaderSize + cipherLen);

    std::lock_guard<std::mutex> lock(sendLock_);
    if (sock_ < 0) { return; }
    // MSG_DONTWAIT: a full socket buffer under Wi-Fi power-save must drop the
    // datagram, not stall the SDL input thread (android saw a blocking sendto
    // wedge the hot path for 1.5 s). Loss is fine — every stream is loss-safe
    // by design.
    ::sendto(sock_, packet.data(), packet.size(), MSG_NOSIGNAL | MSG_DONTWAIT,
             reinterpret_cast<sockaddr*>(&dest_), sizeof(dest_));
}

void SatelliteClient::startHeartbeat() {
    if (heartbeatRunning_.exchange(true)) { return; }
    missedAcks_.store(0, std::memory_order_relaxed);
    connectionAlive_.store(true, std::memory_order_relaxed);
    heartbeatThread_ = std::thread([this] { heartbeatLoop(); });
}

void SatelliteClient::stopHeartbeat() {
    if (!heartbeatRunning_.exchange(false)) { return; }
    if (heartbeatThread_.joinable()) { heartbeatThread_.join(); }
}

void SatelliteClient::heartbeatLoop() {
    using namespace std::chrono;
    while (heartbeatRunning_.load(std::memory_order_relaxed)) {
        sendEncrypted(kMsgHeartbeatPing, nullptr, 0);
        // The RTT clock starts here, on this session's own stamp. Arm only when
        // no ping is outstanding (or the outstanding one aged past the loss
        // cap) — overwriting would pair a late ack with the newer ping's stamp
        // and read artificially low (reducer::shouldArmPing).
        const std::int64_t now = nowSteadyUs();
        if (reducer::shouldArmPing(pingSentUs_.load(std::memory_order_relaxed), now)) {
            pingSentUs_.store(now, std::memory_order_relaxed);
        }
        const int missed = missedAcks_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (missed >= kHeartbeatMissMax) {
            connectionAlive_.store(false, std::memory_order_relaxed);
        }
        // Sleep in 100ms chunks so stopHeartbeat() returns promptly.
        auto slept = 0U;
        while (heartbeatRunning_.load(std::memory_order_relaxed) && slept < kHeartbeatIntervalMs) {
            std::this_thread::sleep_for(milliseconds(100));
            slept += 100;
        }
    }
}

void SatelliteClient::startReceiveLoop() {
    if (ackRunning_.exchange(true)) { return; }
    ackThread_ = std::thread([this] { receiveLoop(); });
}

void SatelliteClient::stopReceiveLoop() {
    if (!ackRunning_.exchange(false)) { return; }
    if (ackThread_.joinable()) { ackThread_.join(); }
}

void SatelliteClient::receiveLoop() {
    std::uint8_t buf[256];
    while (ackRunning_.load(std::memory_order_relaxed)) {
        if (sock_ < 0) { break; }
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        const ssize_t n =
            ::recvfrom(sock_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) {
            continue; // EAGAIN/EWOULDBLOCK on RCVTIMEO
        }
        processIncoming(buf, static_cast<std::size_t>(n));
    }
}

void SatelliteClient::processIncoming(const std::uint8_t* buf, std::size_t n) {
    if (n < kHeaderSize + kAuthTag) { return; }
    if (std::memcmp(buf, token_.data(), 4) != 0) { return; }

    const std::uint32_t counter = util::readU32Be(buf + 4);
    // Per-direction replay guard (server→client): drop counter <= last seen
    // (first packet exempt while lastRecvCounter_ == 0).
    if (lastRecvCounter_ != 0 && counter <= lastRecvCounter_) { return; }

    std::vector<std::uint8_t> plain(n - kHeaderSize);
    unsigned long long plainLen = 0;
    if (!wire::decryptPacket(key_.data(), wire::kDirServerToClient, counter, tokenBe_,
                             buf + kHeaderSize, n - kHeaderSize, plain.data(), &plainLen)) {
        return;
    }
    lastRecvCounter_ = counter;
    if (plainLen < 4) { return; }
    const std::uint16_t msgType = readU16Be(plain.data());
    // Inner payload starts after the 4-byte type+length header.
    const std::uint8_t* body = plain.data() + 4;
    const std::size_t bodyLen = static_cast<std::size_t>(plainLen) - 4;

    if (msgType == kMsgHeartbeatAck) {
        // Pair the ack with the in-flight ping: consume the clock (exchange, so
        // a duplicate ack can't double-count) and slide the RTT into the
        // window. push() itself drops a sample past the loss cap — a reclaimed
        // ping's stale ack never skews the median.
        const std::int64_t sent = pingSentUs_.exchange(0, std::memory_order_relaxed);
        if (sent != 0) {
            const double rttMs = static_cast<double>(nowSteadyUs() - sent) / 1000.0;
            std::lock_guard<std::mutex> lock(latencyMtx_);
            latencyWindow_.push(rttMs);
        }
        missedAcks_.store(0, std::memory_order_relaxed);
        connectionAlive_.store(true, std::memory_order_relaxed);
        if (const auto ack = parseHeartbeatAck(body, bodyLen)) {
            backendAvailable_.store(ack->backendAvailable ? 1 : 0, std::memory_order_relaxed);
            activeControllerCount_.store(static_cast<std::int8_t>(ack->totalActiveControllers),
                                         std::memory_order_relaxed);
            serverEpoch_.store(static_cast<std::int32_t>(ack->epoch), std::memory_order_relaxed);
            serverBitmap_.store(static_cast<std::int32_t>(ack->activeBitmap),
                                std::memory_order_relaxed);
        }
    } else if (msgType == kMsgRumble) {
        const auto rm = parseRumbleMessage(body, bodyLen);
        if (!rm) { return; }
        RumbleHandler handler;
        {
            std::lock_guard<std::mutex> lock(rumbleHandlerMtx_);
            handler = rumbleHandler_;
        }
        if (handler) { handler(*rm); }
    } else if (msgType == kMsgLightbar) {
        const auto lm = parseLightbarMessage(body, bodyLen);
        if (!lm) { return; }
        LightbarHandler handler;
        {
            std::lock_guard<std::mutex> lock(lightbarHandlerMtx_);
            handler = lightbarHandler_;
        }
        if (handler) { handler(*lm); }
    } else if (msgType == kMsgSessionClose) {
        if (bodyLen < 1) { return; }
        // Latch the reason and mark dead NOW: the session is already gone
        // server-side, so the main-thread alive-poll doesn't wait out the full
        // heartbeat death window. The session layer maps the reason to a
        // teardown action (drop-key / stay-down / retry) off this thread.
        sessionCloseReason_.store(static_cast<std::int32_t>(body[0]), std::memory_order_relaxed);
        connectionAlive_.store(false, std::memory_order_relaxed);
    }
}

void SatelliteClient::setRumbleHandler(RumbleHandler handler) {
    std::lock_guard<std::mutex> lock(rumbleHandlerMtx_);
    rumbleHandler_ = std::move(handler);
}

void SatelliteClient::setLightbarHandler(LightbarHandler handler) {
    std::lock_guard<std::mutex> lock(lightbarHandlerMtx_);
    lightbarHandler_ = std::move(handler);
}

SatelliteClient::LatencySnapshot SatelliteClient::latencySnapshot() const {
    std::lock_guard<std::mutex> lock(latencyMtx_);
    return {latencyWindow_.oneWayP50Ms(), latencyWindow_.count()};
}

std::optional<SatelliteClient::HeartbeatAck>
SatelliteClient::parseHeartbeatAck(const std::uint8_t* payload, std::size_t len) {
    // backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) +
    // activeBitmap(u16 BE) = 6 bytes. A bare ack from a pre-protocol-1 server
    // is shorter → nullopt (liveness still counts, reconcile doesn't).
    if (payload == nullptr || len < static_cast<std::size_t>(proto::kHeartbeatAckPayloadBytes)) {
        return std::nullopt;
    }
    HeartbeatAck a;
    a.backendAvailable = payload[0] != 0;
    a.totalActiveControllers = payload[1];
    a.epoch = readU16Be(payload + 2);
    a.activeBitmap = readU16Be(payload + 4);
    return a;
}

std::optional<SatelliteClient::LightbarMessage>
SatelliteClient::parseLightbarMessage(const std::uint8_t* payload, std::size_t len) {
    // ctrlIdx + r + g + b = 4 bytes exactly.
    if (payload == nullptr || len < 4) { return std::nullopt; }
    LightbarMessage lm;
    lm.controllerIndex = payload[0];
    lm.r = payload[1];
    lm.g = payload[2];
    lm.b = payload[3];
    return lm;
}

std::optional<SatelliteClient::RumbleMessage>
SatelliteClient::parseRumbleMessage(const std::uint8_t* payload, std::size_t len) {
    // Fixed 7-byte payload: ctrlIdx + strong + weak + dur.
    if (payload == nullptr || len < kRumblePayloadLen) { return std::nullopt; }
    RumbleMessage rm;
    rm.controllerIndex = payload[0];
    rm.strongMagnitude = util::readU16Be(payload + 1);
    rm.weakMagnitude = util::readU16Be(payload + 3);
    rm.durationMs = util::readU16Be(payload + 5);
    return rm;
}

} // namespace dish::net
