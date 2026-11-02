// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightControlStream.h"

#include <enet/enet.h>

#include <poll.h>

#include <chrono>
#include <cstddef>
#include <vector>

namespace dish::source::moon {
namespace {

std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// One process-wide ENet runtime, alive from first use to exit. False when it
// never came up, which every start then reports as its own failure.
bool ensureEnetInitialized() {
    static const bool initialized = [] { return enet_initialize() == 0; }();
    return initialized;
}

constexpr std::int64_t kPingIntervalMs = 500;
constexpr std::int64_t kConnectTimeoutMs = 5000;
constexpr std::uint8_t kChannel = 0;
constexpr std::size_t kChannelCount = 1;
constexpr int kServicePollMs = 2;

// ENet reads every datagram into a buffer this size, so a packet it did not fragment always fits.
constexpr std::size_t kDatagramBytes = ENET_PROTOCOL_MAXIMUM_MTU;

// ENet 1.3.18's enet_peer_has_outgoing_commands, which the 1.3.17 fork vendored here predates: a
// reliable command queued, or sent and not yet acknowledged.
bool hasOutgoingCommands(const ENetPeer* peer) {
    return !enet_list_empty(&peer->outgoingCommands) ||
           !enet_list_empty(&peer->sentReliableCommands);
}

} // namespace

// Guarded by linkMtx_ like the rest of the link state; see the header for why
// the ENet types are kept out of it.
struct MoonlightControlStream::Link {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
};

MoonlightControlStream::MoonlightControlStream() : link_(std::make_unique<Link>()) {}

MoonlightControlStream::~MoonlightControlStream() { stop(false); }

bool MoonlightControlStream::start(const std::string& hostAddress, std::uint16_t port,
                                   std::uint32_t connectData,
                                   const std::array<std::uint8_t, 16>& rikey) {
    stop(false);
    if (!ensureEnetInitialized()) { return false; }

    ENetAddress address{};
    if (enet_address_set_host(&address, hostAddress.c_str()) != 0) { return false; }
    enet_address_set_port(&address, port);

    ENetSocket socket = ENET_SOCKET_NULL;
    {
        std::lock_guard<thread::TrackedMutex> lock(linkMtx_);
        if (!cipher_.setKey(rikey)) { return false; }
        seq_ = 0;
        for (auto& slot : slots_) { slot.inUse = false; }
        nextSlot_ = 0;

        link_->host = enet_host_create(address.address.ss_family, nullptr, 1, kChannelCount, 0, 0);
        if (link_->host == nullptr) { return false; }
        link_->peer = enet_host_connect(link_->host, &address, kChannelCount, connectData);
        if (link_->peer == nullptr) {
            enet_host_destroy(link_->host);
            link_->host = nullptr;
            return false;
        }
        socket = link_->host->socket;
    }
    lastPingMs_ = steadyNowMs();
    connectDeadlineMs_ = steadyNowMs() + kConnectTimeoutMs;

    stopRequested_.store(false, std::memory_order_relaxed);
    connected_.store(false, std::memory_order_relaxed);
    awaitingAck_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    // The socket stays open until stop() destroys the host, after joining this thread.
    thread_ = std::thread([this, socket] { serviceLoop(socket); });
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
            std::lock_guard<thread::TrackedMutex> lock(linkMtx_);
            if (link_->peer != nullptr) {
                thread::blockingRegion("enet.teardown");
                enet_peer_disconnect_now(link_->peer, 0);
                link_->peer = nullptr;
            }
        }
        stopRequested_.store(true, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
        thread_.join();
    }
    std::lock_guard<thread::TrackedMutex> lock(linkMtx_);
    if (link_->host != nullptr) {
        thread::blockingRegion("enet.teardown");
        enet_host_destroy(link_->host);
        link_->host = nullptr;
    }
    link_->peer = nullptr;
    connected_.store(false, std::memory_order_relaxed);
}

void MoonlightControlStream::notifyLink(bool connected) {
    if (linkHandler_) { linkHandler_(connected); }
}

// One service call under the link lock, the lock every hot-path send takes: it never waits inside
// it, and a packet is only copied out here, never opened.
MoonlightControlStream::ServiceStep
MoonlightControlStream::serviceOnce(std::vector<std::uint8_t>& payload) {
    ServiceStep step;
    std::lock_guard<thread::TrackedMutex> lock(linkMtx_);
    if (link_->host == nullptr) {
        step.hostGone = true;
        return step;
    }

    thread::blockingRegion("enet.service");
    ENetEvent event{};
    step.event = enet_host_service(link_->host, &event, 0) > 0;
    awaitingAck_.store(link_->peer != nullptr && hasOutgoingCommands(link_->peer),
                       std::memory_order_relaxed);
    if (step.event) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            connected_.store(true, std::memory_order_relaxed);
            step.linkUp = true;
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            connected_.store(false, std::memory_order_relaxed);
            step.linkDown = true;
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            payload.assign(event.packet->data, event.packet->data + event.packet->dataLength);
            enet_packet_destroy(event.packet);
            step.received = true;
            break;
        case ENET_EVENT_TYPE_NONE:
        default:
            break;
        }
    }

    const bool connectTimedOut = !connected_.load(std::memory_order_relaxed) && !step.linkUp &&
                                 !step.linkDown && steadyNowMs() > connectDeadlineMs_;
    if (connectTimedOut) { step.linkDown = true; }
    return step;
}

bool MoonlightControlStream::serviceDue(int socket) const {
    thread::blockingRegion("enet.wait");
    pollfd ready{};
    ready.fd = socket;
    ready.events = POLLIN;
    // Readable, or an error the next service call should see.
    if (::poll(&ready, 1, kServicePollMs) != 0) { return true; }
    return awaitingAck_.load(std::memory_order_relaxed) ||
           !connected_.load(std::memory_order_relaxed);
}

// Keep-alive. The clock is the service thread's own; the lock is taken only to send.
void MoonlightControlStream::pingIfDue() {
    if (!connected_.load(std::memory_order_relaxed)) { return; }
    const std::int64_t now = steadyNowMs();
    if (now - lastPingMs_ < kPingIntervalMs) { return; }
    lastPingMs_ = now;
    std::uint8_t plaintext[moonwire::kMaxPlaintextSize];
    const std::size_t len = moonwire::encodePeriodicPing(plaintext);
    sealAndSend(plaintext, len);
}

// A packet that will not open, or opens into an event this version does not model, is dropped: the
// host is entitled to send events a client need not understand.
void MoonlightControlStream::dispatchReceived(const std::vector<std::uint8_t>& payload) {
    std::uint8_t plaintext[256];
    const auto opened = cipher_.open(payload.data(), payload.size(), plaintext, sizeof(plaintext));
    if (!opened) { return; }
    const auto decoded = moonwire::decodeHostEvent(plaintext, *opened);
    if (decoded && eventHandler_) { eventHandler_(*decoded); }
}

void MoonlightControlStream::serviceLoop(int socket) {
    const thread::ScopedThreadRole role(thread::ThreadRole::MoonlightRx);
    std::vector<std::uint8_t> payload;
    payload.reserve(kDatagramBytes);
    bool announcedConnect = false;
    bool drained = false;
    while (running_.load(std::memory_order_relaxed)) {
        pingIfDue();
        if (drained && !serviceDue(socket)) { continue; }
        const ServiceStep step = serviceOnce(payload);
        if (step.hostGone) { break; }
        drained = !step.event;

        // Everything below runs with the lock released, so a handler may call back into a sender
        // without deadlocking, and opening a packet never holds up a send.
        if (step.linkUp && !announcedConnect) {
            announcedConnect = true;
            notifyLink(true);
        }
        if (step.received) { dispatchReceived(payload); }
        if (step.linkDown) {
            if (!stopRequested_.load(std::memory_order_relaxed)) { notifyLink(false); }
            running_.store(false, std::memory_order_relaxed);
            break;
        }
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
            // Runs inside an enet call, which this class only makes under
            // linkMtx_, so the flag flip is already serialized.
            packet->freeCallback = [](ENetPacket* freed) {
                auto* released = static_cast<Slot*>(freed->userData);
                if (released != nullptr) { released->inUse = false; }
            };
            slot->inUse = true;
        }
    } else {
        // Ring exhausted (deep unacked backlog): fall back to a copying send
        // rather than dropping input.
        packet = enet_packet_create(sealed, sealedLen,
                                    static_cast<enet_uint32>(ENET_PACKET_FLAG_RELIABLE));
    }
    if (packet == nullptr) { return false; }
    if (enet_peer_send(link_->peer, kChannel, packet) < 0) {
        enet_packet_destroy(packet);
        return false;
    }
    awaitingAck_.store(true, std::memory_order_relaxed);
    // Push the datagram out on THIS thread: input latency stays flat instead
    // of waiting for the next service tick.
    thread::blockingRegion("enet.flush");
    enet_host_flush(link_->host);
    return true;
}

bool MoonlightControlStream::linkUpLocked() const {
    return link_->host != nullptr && link_->peer != nullptr &&
           connected_.load(std::memory_order_relaxed);
}

void MoonlightControlStream::sealAndSend(const std::uint8_t* plaintext, std::size_t len) {
    std::lock_guard<thread::TrackedMutex> lock(linkMtx_);
    if (!linkUpLocked()) { return; }
    sealAndQueueLocked(plaintext, len);
}

void MoonlightControlStream::sealAndQueueLocked(const std::uint8_t* plaintext, std::size_t len) {
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

void MoonlightControlStream::sendControllerReplug(std::uint8_t controllerNumber,
                                                  std::uint16_t otherPadsMask,
                                                  std::uint8_t controllerType,
                                                  std::uint8_t capabilities,
                                                  std::uint32_t supportedButtons) {
    std::uint8_t unplug[moonwire::kMaxPlaintextSize];
    const std::size_t unplugLen = moonwire::encodeControllerMulti(
        unplug, controllerNumber, otherPadsMask, 0, 0, 0, 0, 0, 0, 0);
    std::uint8_t arrival[moonwire::kMaxPlaintextSize];
    const std::size_t arrivalLen = moonwire::encodeControllerArrival(
        arrival, controllerNumber, controllerType, capabilities, supportedButtons);
    std::lock_guard<thread::TrackedMutex> lock(linkMtx_);
    if (!linkUpLocked()) { return; }
    sealAndQueueLocked(unplug, unplugLen);
    sealAndQueueLocked(arrival, arrivalLen);
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
