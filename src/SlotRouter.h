// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Input/GamepadInputProcessor.h"
#include "Network/SatelliteClient.h"
#include "core/audio/PadAudioMatcher.h"
#include "core/input/UsbReportParsers.h"
#include "core/reducer/FeedbackRouting.h"
#include "core/thread/GraceEpoch.h"
#include "core/thread/TrackedMutex.h"
#include "source/usb/UsbGamepadManager.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dish {

class FeatureSettings;

namespace input {
class SDLGamepadBridge;
}

namespace net {
class ConnectionHub;
class WifiConnection;
class WifiConnectionManager;
} // namespace net

namespace source {
class MotionEnabledStore;
class RumbleEnabledStore;
namespace audio {
class SpeakerPlayoutEngine;
}
} // namespace source

// Where every pad's reports and mic frames go and where every session's feedback lands, as one
// immutable table the GUI thread builds and publishes and every input, receive and capture thread
// reads without a lock. A reader brackets each use in a grace epoch on a participant of its own (a
// device's index for the thread that publishes that device, a session's own for its receive
// thread, a mic sender's own for its capture thread), so the GUI can retire the table it replaced
// once no bracket holds it. AppModel and the hot-path harness use it alike.
class SlotRouter final : public input::GamepadInputProcessor::InputSink {
  public:
    using DeviceIndex = input::GamepadInputProcessor::DeviceIndex;
    using DeviceId = input::GamepadInputProcessor::DeviceId;
    using DeviceState = input::GamepadInputProcessor::DeviceState;
    using MotionSample = input::GamepadInputProcessor::MotionSample;
    using BatterySample = input::GamepadInputProcessor::BatterySample;
    using TouchpadSample = input::GamepadInputProcessor::TouchpadSample;

    struct Dependencies {
        const net::ConnectionHub* hub = nullptr;
        net::WifiConnectionManager* wifi = nullptr;
        input::GamepadInputProcessor* processor = nullptr;
        input::SDLGamepadBridge* bridge = nullptr;
        source::usb::UsbGamepadManager* usbManager = nullptr;
        source::MotionEnabledStore* motionEnabled = nullptr;
        source::RumbleEnabledStore* rumbleEnabled = nullptr;
        FeatureSettings* features = nullptr;
        source::audio::SpeakerPlayoutEngine* speaker = nullptr;
    };

    // What the pad behind a slot can do, by the path it is on.
    struct SlotHardware {
        bool usbDirect = false;
        bool hasMotion = false;
        bool hasLightbar = false;
        bool hasTouchpad = false;
        bool hasRumble = false;
        bool hasTriggerEffects = false;
        bool hasPlayerLeds = false;
        bool hasMicLed = false;
        // SDL_GameControllerSendEffect lands only in SDL's own HIDAPI drivers.
        bool sdlEffects = false;
        // The pad sources the protocol 4 extended-buttons word: the model table
        // for a Direct pad, SDL's mapping for a Standard one.
        bool hasExtendedButtons = false;
    };

    // Where a slot bound to a Moonlight host sends, its pad number resolved when it bound, and the
    // host and number its feedback names the pad by.
    struct MoonlightRoutes {
        std::function<void(const DeviceState&)> report;
        std::function<void(const MotionSample&)> motion;
        std::function<void(const BatterySample&)> battery;
        std::function<void(const TouchpadSample&)> touch;
        std::string hostUuid;
        std::uint8_t controllerNumber = 0;
    };

    explicit SlotRouter(const Dependencies& deps);
    ~SlotRouter() override;

    SlotRouter(const SlotRouter&) = delete;
    SlotRouter& operator=(const SlotRouter&) = delete;
    SlotRouter(SlotRouter&&) = delete;
    SlotRouter& operator=(SlotRouter&&) = delete;

    // GUI thread, before any input thread starts: the processor's reports leave through here.
    void attach();
    // GUI thread: a fresh table for the slots on screen; each hot thread reads it from its next
    // report or message on.
    void republish(const QStringList& slotIds);
    // GUI thread, once per session: the handlers its receive thread calls.
    void installFeedbackHandlers(net::WifiConnection& conn, const QString& connectionId);
    // GUI thread: a slot's Moonlight senders, or none (empty routes) to take them away. A slot
    // with a session route sends there and never here.
    void setMoonlightRoutes(const QString& slotId, MoonlightRoutes routes);
    // A Moonlight host's feedback for the pad it calls `controllerNumber`, on the host's control
    // stream thread: routed to the pad's mailbox or its Direct worker, with the user's rumble
    // switch and light-bar setting applied, and no hop through the GUI thread.
    void moonlightRumble(std::string_view hostUuid, std::uint8_t controllerNumber,
                         std::uint16_t strong, std::uint16_t weak);
    void moonlightLightbar(std::string_view hostUuid, std::uint8_t controllerNumber, std::uint8_t r,
                           std::uint8_t g, std::uint8_t b);

    // seq, the Opus packet and its length; true when the frame was sent.
    using MicSender = std::function<bool(std::uint16_t, const std::uint8_t*, std::size_t)>;
    // One participant per bit of the word that hands them out.
    static constexpr std::size_t kMicSenders = std::numeric_limits<std::uint64_t>::digits;
    // GUI thread: how one slot's mic frames leave its capture thread. Each frame goes where the
    // table the GUI last published routes the slot, read inside a bracket on a participant the
    // sender owns, so one thread at a time calls a sender; the participant comes back when the
    // sender's last copy goes. Empty while kMicSenders are alive. Kept past the router, a sender
    // sends nothing.
    MicSender micSenderForSlot(const QString& slotId) const;

    // GUI thread, answered live: the descriptor's caps and the actuation the GUI starts itself.
    SlotHardware slotHardware(const QString& slotId) const;
    reducer::SlotFeedbackInputs feedbackInputs(const QString& slotId) const;
    audio::PadAudioRoute audioRouteForSlot(const QString& slotId) const;
    // True when the routes changed.
    bool setPadAudioRoutes(std::map<int, audio::PadAudioRoute> routes);
    // With the user's rumble switch applied.
    void deliverRumble(const QString& slotId, std::uint16_t strong, std::uint16_t weak,
                       std::uint16_t durationMs);
    void actuateRumble(const QString& slotId, std::uint16_t strong, std::uint16_t weak,
                       std::uint16_t durationMs);
    void actuateLightbar(const QString& slotId, std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void
    actuateTriggerEffects(const QString& slotId,
                          const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& left,
                          const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& right);
    void actuatePlayerLeds(const QString& slotId, std::uint8_t ledMask);
    void actuateMicLed(const QString& slotId, std::uint8_t state);
    // GUI thread: a battery heartbeat for a slot whose reports another thread publishes (a Direct
    // pad's routed battery), sent from the published table under the publishers' lock.
    void sendBattery(const QString& slotId, std::uint8_t level, std::uint8_t status);

    // Feedback the receive threads routed to a pad, for the harness.
    std::uint64_t rumblesRouted() const { return rumblesRouted_.load(std::memory_order_relaxed); }
    std::uint64_t lightbarsRouted() const {
        return lightbarsRouted_.load(std::memory_order_relaxed);
    }

    // InputSink, on each device's own thread.
    void report(DeviceIndex index, const DeviceId& id, const DeviceState& state) override;
    void motion(DeviceIndex index, const DeviceId& id, const MotionSample& sample,
                std::uint32_t timestampDeltaUs) override;
    void battery(DeviceIndex index, const DeviceId& id, const BatterySample& sample) override;
    void touchpad(DeviceIndex index, const DeviceId& id, const TouchpadSample& sample) override;

  private:
    static constexpr std::size_t kFeedbackKinds = 5;
    static constexpr std::size_t kDeviceParticipants = input::GamepadInputProcessor::kMaxDevices;
    static constexpr std::size_t kSessionParticipants = 64;
    using Grace = thread::GraceEpoch<kDeviceParticipants + kSessionParticipants>;

    struct InputRoute {
        std::shared_ptr<net::SatelliteClient> client;
        int controllerIndex = 0;
        bool motion = false;
    };
    struct FeedbackRoute {
        QString connectionId;
        QString slotId;
        std::array<reducer::FeedbackTarget, kFeedbackKinds> targets{};
        std::optional<source::usb::UsbGamepadManager::DirectTarget> direct;
        // The SDL pad's mailbox, for the Standard path.
        std::optional<DeviceIndex> padIndex;
        bool rumbleOn = true;

        reducer::FeedbackTarget target(reducer::FeedbackKind kind) const {
            return targets[static_cast<std::size_t>(kind)];
        }
    };
    struct MicRoute {
        std::string slotId;
        std::shared_ptr<net::SatelliteClient> client;
        int controllerIndex = 0;
    };
    struct RouteTable {
        std::array<InputRoute, kDeviceParticipants> inputs;
        std::vector<MicRoute> mics;
        std::vector<FeedbackRoute> feedback;
        const FeedbackRoute* feedbackFor(const QString& connectionId) const;
        const MicRoute* micFor(const std::string& slotId) const;
    };
    // What the mic senders share with the router and keep past it: the published table's address
    // and the brackets that guard it, with a participant taken for each sender alive.
    struct MicShare {
        thread::GraceEpoch<kMicSenders> grace;
        std::atomic<const RouteTable*> table{nullptr};
        std::atomic<std::uint64_t> taken{0};
    };
    class MicLease;

    std::unique_ptr<const RouteTable> build(const QStringList& slotIds) const;
    // The Moonlight sender `member` names for a device, copied out under moonlight.routes; none
    // while no slot is bound, which costs one relaxed load.
    template <typename Sender>
    Sender moonlightSender(const DeviceId& id, Sender MoonlightRoutes::* member) const;
    // GUI thread: each Moonlight-bound slot's feedback route, rebuilt live.
    void refreshMoonlightFeedback();
    static SlotHardware syntheticHardware(int vendorId, int productId);
    SlotHardware sdlHardware(const QString& slotId) const;

    // The receive thread's half: the session's route, read inside a bracket on its participant.
    template <typename Deliver>
    void withFeedbackRoute(std::size_t participant, const QString& connectionId, Deliver&& deliver);
    void routeRumble(const FeedbackRoute& route, std::uint16_t strong, std::uint16_t weak,
                     std::uint16_t durationMs);
    void routeLightbar(const FeedbackRoute& route, std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void
    routeTriggerEffects(const FeedbackRoute& route,
                        const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& left,
                        const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& right);
    void routePlayerLeds(const FeedbackRoute& route, std::uint8_t ledMask);
    void routeMicLed(const FeedbackRoute& route, std::uint8_t state);
    // The GUI's own actuation: the same routing over a route built live for the one slot.
    FeedbackRoute liveRoute(const QString& slotId) const;

    const Dependencies deps_;
    Grace grace_;
    // Taken by publishers only: the GUI's republish and its battery heartbeat.
    thread::TrackedMutex routesMtx_{"app.routes"};
    std::unique_ptr<const RouteTable> published_;
    std::atomic<const RouteTable*> table_{nullptr};
    const std::shared_ptr<MicShare> mics_ = std::make_shared<MicShare>();
    // GUI thread only.
    QHash<QString, std::size_t> sessionParticipants_;
    // The matcher's answer, written and read on the GUI thread.
    mutable thread::TrackedMutex audioRoutesMtx_{"app.audioRoutes"};
    std::map<int, audio::PadAudioRoute> padAudioRoutes_;
    std::atomic<std::uint64_t> rumblesRouted_{0};
    std::atomic<std::uint64_t> lightbarsRouted_{0};
    // Written on the GUI thread, read by the input threads and the Moonlight control threads.
    struct MoonlightFeedback {
        std::string hostUuid;
        std::uint8_t controllerNumber = 0;
        FeedbackRoute route;
    };
    // The route a Moonlight host's feedback for one pad takes, copied out under moonlight.routes.
    std::optional<FeedbackRoute> moonlightFeedbackRoute(std::string_view hostUuid,
                                                        std::uint8_t controllerNumber) const;
    mutable thread::TrackedMutex moonlightMtx_{"moonlight.routes"};
    std::map<std::string, MoonlightRoutes, std::less<>> moonlightRoutes_;
    std::vector<MoonlightFeedback> moonlightFeedback_;
    std::atomic<bool> anyMoonlight_{false};
};

} // namespace dish
