// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Moonlight control-stream link: an ENet client connection carrying
// AES-GCM-sealed control messages both ways. Owns the service thread that
// pumps ENet, decrypts host events (rumble, trigger rumble, motion requests,
// RGB LED, termination) and sends the periodic keep-alive ping.
//
// Hot path: sendControllerMulti runs on the SDL input thread. It encodes and
// seals into a preallocated ring slot under the link mutex and hands ENet the
// slot's buffer with ENET_PACKET_FLAG_NO_ALLOCATE, so this layer performs no
// per-packet heap allocation and no payload copies. (ENet itself still
// allocates its small ENetPacket bookkeeping struct per send; that lives in
// the vendored library.) The GCM context is reused across packets.

#pragma once

#include "core/moonlight/MoonlightControlCipher.h"
#include "core/moonlight/MoonlightWire.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Mirrors ENet's typedefs so <enet/enet.h> stays out of this header, the same
// way SDLGamepadBridge.h mirrors SDL2's. Including the real header is not an
// option here: dish_enet is linked PRIVATE to dish_core (its include directory
// therefore does not reach the library's consumers), and this header is pulled
// in by MoonlightSession.h -> MoonlightManager.h -> AppModel.h, so it would
// drag <sys/socket.h>, <netinet/in.h> and every ENET_ macro into the whole
// tree. The leading underscores are ENet's struct tags, not our choice.
extern "C" {
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
using ENetHost = struct _ENetHost;
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
using ENetPeer = struct _ENetPeer;
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
using ENetPacket = struct _ENetPacket;
}

namespace dish::source::moon {

class MoonlightControlStream {
  public:
    // Both handlers run on the service thread; marshal in the owner.
    using EventHandler = std::function<void(const moonwire::HostEvent&)>;
    // true once the ENet connect completes; false when the link is lost (a
    // failed connect, a disconnect event, or a service error).
    using LinkHandler = std::function<void(bool connected)>;

    MoonlightControlStream();
    ~MoonlightControlStream();

    MoonlightControlStream(const MoonlightControlStream&) = delete;
    MoonlightControlStream& operator=(const MoonlightControlStream&) = delete;
    MoonlightControlStream(MoonlightControlStream&&) = delete;
    MoonlightControlStream& operator=(MoonlightControlStream&&) = delete;

    // Install before start(); not thread-safe against a running stream.
    void setEventHandler(EventHandler handler) { eventHandler_ = std::move(handler); }
    void setLinkHandler(LinkHandler handler) { linkHandler_ = std::move(handler); }

    // Dials `hostAddress:port`, passing `connectData` (the RTSP SETUP
    // X-SS-Connect-Data) in the connect packet, keyed with the launch rikey.
    // False when ENet setup fails outright; connect success/failure is then
    // reported through the link handler.
    bool start(const std::string& hostAddress, std::uint16_t port, std::uint32_t connectData,
               const std::array<std::uint8_t, 16>& rikey);

    // Graceful teardown: optionally sends TERMINATION, then disconnects and
    // joins the service thread. Idempotent.
    void stop(bool sendTermination);

    bool isConnected() const { return connected_.load(std::memory_order_relaxed); }

    // ── Senders (any thread; dropped silently while unconnected) ─────────────
    void sendControllerMulti(std::uint8_t controllerNumber, std::uint16_t activeMask,
                             std::uint32_t buttonFlags, std::uint8_t leftTrigger,
                             std::uint8_t rightTrigger, std::int16_t leftX, std::int16_t leftY,
                             std::int16_t rightX, std::int16_t rightY);
    void sendControllerArrival(std::uint8_t controllerNumber, std::uint8_t controllerType,
                               std::uint8_t capabilities, std::uint32_t supportedButtons);
    void sendControllerMotion(std::uint8_t controllerNumber, std::uint8_t motionType, float x,
                              float y, float z);
    void sendControllerBattery(std::uint8_t controllerNumber, std::uint8_t state,
                               std::uint8_t percentage);

  private:
    // A sealed-packet slot ENet borrows until delivery. Sized for the largest
    // plaintext plus the GCM framing.
    struct Slot {
        std::array<std::uint8_t, 128> buffer{};
        bool inUse = false;
    };
    static constexpr std::size_t kSlotCount = 32;

    static void releaseSlot(ENetPacket* packet);

    void serviceLoop();
    void notifyLink(bool connected);
    // Seals `plaintext` and queues it. Caller must NOT hold linkMtx_.
    void sealAndSend(const std::uint8_t* plaintext, std::size_t len);
    // linkMtx_ held. Sends the pre-sealed slot/fallback packet.
    bool queuePacket(const std::uint8_t* sealed, std::size_t sealedLen, Slot* slot);

    EventHandler eventHandler_;
    LinkHandler linkHandler_;

    mutable std::mutex linkMtx_; // guards everything below
    ENetHost* host_ = nullptr;
    ENetPeer* peer_ = nullptr;
    mooncrypto::ControlCipher cipher_;
    std::uint32_t seq_ = 0;
    std::array<Slot, kSlotCount> slots_{};
    std::size_t nextSlot_ = 0;
    std::int64_t lastPingMs_ = 0;
    std::int64_t connectDeadlineMs_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopRequested_{false};
};

} // namespace dish::source::moon
