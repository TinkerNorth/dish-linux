// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SatelliteClient.h"

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

    // 500 ms recv timeout so the ACK loop can poll `ackRunning_` cleanly.
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
                                          const std::array<std::uint8_t, 32>& key) {
    token_ = token;
    key_ = key;
    counter_.reset();
    missedAcks_.store(0, std::memory_order_relaxed);
    connectionAlive_.store(true, std::memory_order_relaxed);
    lastControllerAck_.store(-1, std::memory_order_relaxed);
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

void SatelliteClient::controllerAdd(int index, std::uint16_t capabilities) {
    std::uint8_t payload[3]{};
    payload[0] = static_cast<std::uint8_t>(index);
    putU16Be(&payload[1], capabilities);
    sendEncrypted(kMsgControllerAdd, payload, sizeof(payload));
}

void SatelliteClient::controllerRemove(int index) {
    const std::uint8_t payload[1] = {static_cast<std::uint8_t>(index)};
    sendEncrypted(kMsgControllerRemove, payload, sizeof(payload));
}

void SatelliteClient::sendControllerType(int index, int type) {
    const std::uint8_t payload[2] = {static_cast<std::uint8_t>(index),
                                     static_cast<std::uint8_t>(type)};
    sendEncrypted(kMsgControllerType, payload, sizeof(payload));
}

void SatelliteClient::sendEncrypted(std::uint16_t msgType, const std::uint8_t* payload,
                                    std::size_t len) {
    if (sock_ < 0) { return; }
    // Inner: msgType(BE16) + payloadLen(BE16) + payload
    const std::size_t innerLen = 4 + len;
    std::vector<std::uint8_t> inner(innerLen);
    putU16Be(inner.data(), msgType);
    putU16Be(inner.data() + 2, static_cast<std::uint16_t>(len));
    if (len > 0) { std::memcpy(inner.data() + 4, payload, len); }

    const auto ctr = static_cast<std::uint32_t>(counter_.next());

    std::uint8_t nonce[12] = {0};
    putU32Be(&nonce[8], ctr);

    // Packet: token(4) + counter(4) + ciphertext + 16-byte tag.
    std::vector<std::uint8_t> packet(8 + innerLen + crypto_aead_chacha20poly1305_IETF_ABYTES);
    std::memcpy(packet.data(), token_.data(), 4);
    putU32Be(packet.data() + 4, ctr);

    unsigned long long cipherLen = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(packet.data() + 8, &cipherLen, inner.data(),
                                                  inner.size(), token_.data(), token_.size(),
                                                  nullptr, nonce, key_.data()) != 0) {
        return;
    }
    packet.resize(8 + cipherLen);

    std::lock_guard<std::mutex> lock(sendLock_);
    if (sock_ < 0) { return; }
    ::sendto(sock_, packet.data(), packet.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&dest_),
             sizeof(dest_));
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
    if (n < 8 + crypto_aead_chacha20poly1305_IETF_ABYTES) { return; }
    if (std::memcmp(buf, token_.data(), 4) != 0) { return; }

    std::uint8_t nonce[12] = {0};
    std::memcpy(&nonce[8], buf + 4, 4);

    std::vector<std::uint8_t> plain(n - 8);
    unsigned long long plainLen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(plain.data(), &plainLen, nullptr, buf + 8, n - 8,
                                                  token_.data(), token_.size(), nonce,
                                                  key_.data()) != 0) {
        return;
    }
    if (plainLen < 4) { return; }
    const std::uint16_t msgType = util::readU16Be(plain.data());
    const std::uint16_t msgLen = util::readU16Be(plain.data() + 2);

    if (msgType == kMsgHeartbeatAck) {
        missedAcks_.store(0, std::memory_order_relaxed);
        connectionAlive_.store(true, std::memory_order_relaxed);
    } else if (msgType == kMsgControllerAck && msgLen >= 4 && plainLen >= 8) {
        const std::uint16_t reqType = util::readU16Be(plain.data() + 4);
        const std::uint8_t idx = plain[6];
        const std::uint8_t result = plain[7];
        const std::int32_t packed = (static_cast<std::int32_t>(reqType) << 16) |
                                    (static_cast<std::int32_t>(idx) << 8) |
                                    static_cast<std::int32_t>(result);
        lastControllerAck_.store(packed, std::memory_order_relaxed);
    } else if (msgType == kMsgServerStatus && msgLen >= 2 && plainLen >= 6) {
        vigemAvailable_.store(plain[4] == 0 ? 0 : 1, std::memory_order_relaxed);
        activeControllerCount_.store(static_cast<std::int8_t>(plain[5]), std::memory_order_relaxed);
    } else if (msgType == kMsgRumble) {
        // The inner header bytes are at plain[0..3]; the payload starts at +4.
        // parseRumbleMessage works on the payload region for parity with the
        // unit-test seam, so adjust the pointer/length accordingly.
        if (plainLen < 4) { return; }
        const auto rm =
            parseRumbleMessage(plain.data() + 4, static_cast<std::size_t>(plainLen) - 4);
        if (!rm) { return; }
        RumbleHandler handler;
        {
            std::lock_guard<std::mutex> lock(rumbleHandlerMtx_);
            handler = rumbleHandler_;
        }
        if (handler) { handler(*rm); }
    }
}

void SatelliteClient::setRumbleHandler(RumbleHandler handler) {
    std::lock_guard<std::mutex> lock(rumbleHandlerMtx_);
    rumbleHandler_ = std::move(handler);
}

std::optional<SatelliteClient::RumbleMessage>
SatelliteClient::parseRumbleMessage(const std::uint8_t* payload, std::size_t len) {
    // Mandatory fields: ctrlIdx + strong + weak + dur + flags = 8 bytes.
    if (payload == nullptr || len < 8) { return std::nullopt; }
    RumbleMessage rm;
    rm.controllerIndex = payload[0];
    rm.strongMagnitude = util::readU16Be(payload + 1);
    rm.weakMagnitude = util::readU16Be(payload + 3);
    rm.durationMs = util::readU16Be(payload + 5);
    const std::uint8_t flags = payload[7];
    rm.hasLightbar = (flags & 0x01) != 0;
    if (rm.hasLightbar) {
        if (len < 11) { return std::nullopt; } // declared lightbar but truncated
        rm.lightbarR = payload[8];
        rm.lightbarG = payload[9];
        rm.lightbarB = payload[10];
    }
    return rm;
}

} // namespace dish::net
