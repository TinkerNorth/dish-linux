// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightControlStream.h"

#include <enet/enet.h>

#include <chrono>
#include <vector>

namespace dish::source::moon {
namespace {

std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// One process-wide ENet runtime, alive from first use to exit.
void ensureEnetInitialized() {
    static const bool initialized = [] { return enet_initialize() == 0; }();
    (void)initialized;
}

constexpr std::int64_t kPingIntervalMs = 500;
constexpr std::int64_t kConnectTimeoutMs = 5000;
constexpr std::uint8_t kChannel = 0;
constexpr std::size_t kChannelCount = 1;

} // namespace

MoonlightControlStream::MoonlightControlStream() = default;

MoonlightControlStream::~MoonlightControlStream() { stop(false); }

void MoonlightControlStream::releaseSlot(ENetPacket* packet) {
    // Runs inside an enet call, which this class only makes under linkMtx_,
    // so the flag flip is already serialized.
    auto* slot = static_cast<Slot*>(packet->userData);
    if (slot != nullptr) { slot->inUse = false; }
}

bool MoonlightControlStream::start(const std::string& hostAddress, std::uint16_t port,
                                   std::uint32_t connectData,
                                   const std::array<std::uint8_t, 16>& rikey) {
    stop(false);
    ensureEnetInitialized();

    ENetAddress address{};
    if (enet_address_set_host(&address, hostAddress.c_str()) != 0) { return false; }
    enet_address_set_port(&address, port);

    {
        std::lock_guard<std::mutex> lock(linkMtx_);
        if (!cipher_.setKey(rikey)) { return false; }
        seq_ = 0;
        for (auto& slot : slots_) { slot.inUse = false; }
        nextSlot_ = 0;

        host_ = enet_host_create(address.address.ss_family, nullptr, 1, kChannelCount, 0, 0);
        if (host_ == nullptr) { return false; }
        peer_ = enet_host_connect(host_, &address, kChannelCount, connectData);
        if (peer_ == nullptr) {
            enet_host_destroy(host_);
            host_ = nullptr;
            return false;
        }
        lastPingMs_ = steadyNowMs();
        connectDeadlineMs_ = steadyNowMs() + kConnectTimeoutMs;
    }

    stopRequested_.store(false, std::memory_order_relaxed);
    connected_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this] { serviceLoop(); });
    return true;
}

void MoonlightControlStream::stop(bool sendTermination) {
    if (thread_.joinable()) {
        if (sendTermination && connected_.load(std::memory_order_relaxed)) {
            std::uint8_t plaintext[moonwire::kMaxPlaintextSize];
            const std::size_t len = moonwire::encodeTermination(plaintext);
            sealAndSend(plaintext, len);
        }
        {
            std::lock_guard<std::mutex> lock(linkMtx_);
            if (peer_ != nullptr) {
                enet_peer_disconnect_now(peer_, 0);
                peer_ = nullptr;
            }
        }
        stopRequested_.store(true, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(linkMtx_);
    if (host_ != nullptr) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    peer_ = nullptr;
    connected_.store(false, std::memory_order_relaxed);
}

void MoonlightControlStream::notifyLink(bool connected) {
    if (linkHandler_) { linkHandler_(connected); }
}

void MoonlightControlStream::serviceLoop() {
    bool announcedConnect = false;
    while (running_.load(std::memory_order_relaxed)) {
        // Decoded events are dispatched after the lock is released, so a
        // handler may call back into a sender without deadlocking.
        std::vector<moonwire::HostEvent> events;
        bool linkUp = false;
        bool linkDown = false;

        {
            std::lock_guard<std::mutex> lock(linkMtx_);
            if (host_ == nullptr) { break; }

            ENetEvent event;
            int guard = 32; // bound one pass; the loop resumes in 2 ms anyway
            while (guard-- > 0 && enet_host_service(host_, &event, 0) > 0) {
                switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    connected_.store(true, std::memory_order_relaxed);
                    linkUp = true;
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    connected_.store(false, std::memory_order_relaxed);
                    linkDown = true;
                    break;
                case ENET_EVENT_TYPE_RECEIVE: {
                    std::uint8_t plaintext[256];
                    const auto len = cipher_.open(event.packet->data, event.packet->dataLength,
                                                  plaintext, sizeof(plaintext));
                    if (len) {
                        if (const auto decoded = moonwire::decodeHostEvent(plaintext, *len)) {
                            events.push_back(*decoded);
                        }
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                case ENET_EVENT_TYPE_NONE:
                default:
                    break;
                }
            }

            const std::int64_t now = steadyNowMs();
            if (!connected_.load(std::memory_order_relaxed) && !linkUp && !linkDown &&
                now > connectDeadlineMs_) {
                linkDown = true; // connect timed out
            }
        }

        if (linkUp && !announcedConnect) {
            announcedConnect = true;
            notifyLink(true);
        }
        for (const auto& ev : events) {
            if (eventHandler_) { eventHandler_(ev); }
        }
        if (linkDown) {
            if (!stopRequested_.load(std::memory_order_relaxed)) { notifyLink(false); }
            running_.store(false, std::memory_order_relaxed);
            break;
        }

        // Keep-alive, off the lock-held section above.
        if (connected_.load(std::memory_order_relaxed)) {
            bool pingDue = false;
            {
                std::lock_guard<std::mutex> lock(linkMtx_);
                const std::int64_t now = steadyNowMs();
                if (now - lastPingMs_ >= kPingIntervalMs) {
                    lastPingMs_ = now;
                    pingDue = true;
                }
            }
            if (pingDue) {
                std::uint8_t plaintext[moonwire::kMaxPlaintextSize];
                const std::size_t len = moonwire::encodePeriodicPing(plaintext);
                sealAndSend(plaintext, len);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool MoonlightControlStream::queuePacket(const std::uint8_t* sealed, std::size_t sealedLen,
                                         Slot* slot) {
    ENetPacket* packet = nullptr;
    if (slot != nullptr) {
        packet = enet_packet_create(nullptr, 0,
                                    static_cast<enet_uint32>(ENET_PACKET_FLAG_RELIABLE) |
                                        static_cast<enet_uint32>(ENET_PACKET_FLAG_NO_ALLOCATE));
        if (packet != nullptr) {
            packet->data = slot->buffer.data();
            packet->dataLength = sealedLen;
            packet->userData = slot;
            packet->freeCallback = &MoonlightControlStream::releaseSlot;
            slot->inUse = true;
        }
    } else {
        // Ring exhausted (deep unacked backlog): fall back to a copying send
        // rather than dropping input.
        packet = enet_packet_create(sealed, sealedLen,
                                    static_cast<enet_uint32>(ENET_PACKET_FLAG_RELIABLE));
    }
    if (packet == nullptr) { return false; }
    if (enet_peer_send(peer_, kChannel, packet) < 0) {
        enet_packet_destroy(packet);
        return false;
    }
    // Push the datagram out on THIS thread: input latency stays flat instead
    // of waiting for the next service tick.
    enet_host_flush(host_);
    return true;
}

void MoonlightControlStream::sealAndSend(const std::uint8_t* plaintext, std::size_t len) {
    std::lock_guard<std::mutex> lock(linkMtx_);
    if (host_ == nullptr || peer_ == nullptr || !connected_.load(std::memory_order_relaxed)) {
        return;
    }

    Slot* slot = nullptr;
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        Slot& candidate = slots_[(nextSlot_ + i) % kSlotCount];
        if (!candidate.inUse) {
            slot = &candidate;
            nextSlot_ = ((nextSlot_ + i) % kSlotCount) + 1;
            break;
        }
    }

    if (slot != nullptr) {
        const std::size_t sealedLen = cipher_.seal(seq_, plaintext, len, slot->buffer.data());
        if (sealedLen == 0) { return; }
        ++seq_;
        queuePacket(slot->buffer.data(), sealedLen, slot);
        return;
    }

    std::uint8_t fallback[moonwire::kMaxPlaintextSize + mooncrypto::ControlCipher::kOverhead];
    const std::size_t sealedLen = cipher_.seal(seq_, plaintext, len, fallback);
    if (sealedLen == 0) { return; }
    ++seq_;
    queuePacket(fallback, sealedLen, nullptr);
}

void MoonlightControlStream::sendControllerMulti(
    std::uint8_t controllerNumber, std::uint16_t activeMask, std::uint32_t buttonFlags,
    std::uint8_t leftTrigger, std::uint8_t rightTrigger, std::int16_t leftX, std::int16_t leftY,
    std::int16_t rightX, std::int16_t rightY) {
    std::uint8_t plaintext[moonwire::kMaxPlaintextSize];
    const std::size_t len =
        moonwire::encodeControllerMulti(plaintext, controllerNumber, activeMask, buttonFlags,
                                        leftTrigger, rightTrigger, leftX, leftY, rightX, rightY);
    sealAndSend(plaintext, len);
}

void MoonlightControlStream::sendControllerArrival(std::uint8_t controllerNumber,
                                                   std::uint8_t controllerType,
                                                   std::uint8_t capabilities,
                                                   std::uint32_t supportedButtons) {
    std::uint8_t plaintext[moonwire::kMaxPlaintextSize];
    const std::size_t len = moonwire::encodeControllerArrival(
        plaintext, controllerNumber, controllerType, capabilities, supportedButtons);
    sealAndSend(plaintext, len);
}

void MoonlightControlStream::sendControllerMotion(std::uint8_t controllerNumber,
                                                  std::uint8_t motionType, float x, float y,
                                                  float z) {
    std::uint8_t plaintext[moonwire::kMaxPlaintextSize];
    const std::size_t len =
        moonwire::encodeControllerMotion(plaintext, controllerNumber, motionType, x, y, z);
    sealAndSend(plaintext, len);
}

void MoonlightControlStream::sendControllerBattery(std::uint8_t controllerNumber,
                                                   std::uint8_t state, std::uint8_t percentage) {
    std::uint8_t plaintext[moonwire::kMaxPlaintextSize];
    const std::size_t len =
        moonwire::encodeControllerBattery(plaintext, controllerNumber, state, percentage);
    sealAndSend(plaintext, len);
}

void MoonlightControlStream::sendControllerTouch(std::uint8_t controllerNumber,
                                                 std::uint8_t eventType, std::uint32_t pointerId,
                                                 float x, float y, float pressure) {
    std::uint8_t plaintext[moonwire::kMaxPlaintextSize];
    const std::size_t len = moonwire::encodeControllerTouch(plaintext, controllerNumber, eventType,
                                                            pointerId, x, y, pressure);
    sealAndSend(plaintext, len);
}

} // namespace dish::source::moon
