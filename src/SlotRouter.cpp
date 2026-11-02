// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SlotRouter.h"

#include "FeatureSettings.h"
#include "Input/SDLGamepadBridge.h"
#include "LightbarRouting.h"
#include "Network/ConnectionHub.h"
#include "Network/WifiConnection.h"
#include "Network/WifiConnectionManager.h"
#include "core/input/UsbOutputReports.h"
#include "core/reducer/BindingPresence.h"
#include "core/reducer/RumbleRouting.h"
#include "core/reducer/SlotPathFields.h"
#include "core/reducer/TouchpadRouting.h"
#include "source/audio/SpeakerPlayoutEngine.h"
#include "source/store/MotionEnabledStore.h"
#include "source/store/RumbleEnabledStore.h"

#include <chrono>
#include <mutex>
#include <string>
#include <utility>

namespace dish {

namespace {

constexpr std::array kFeedbackKindsInOrder{
    reducer::FeedbackKind::Rumble, reducer::FeedbackKind::Lightbar,
    reducer::FeedbackKind::TriggerEffects, reducer::FeedbackKind::PlayerLeds,
    reducer::FeedbackKind::MicLed};

// The protocol-2 actuators and the mic lamp, which the parser family answers for on BOTH paths: a
// Direct claim decodes them from the OUT report, and a pad's USB identity names them whether it
// arrived over USB or Bluetooth. Whether the layer can land them is the next fact, and the
// caller's.
void applyOutputActuators(SlotRouter::SlotHardware& hw, input::usbparse::HidParser parser) {
    hw.hasTriggerEffects = input::usbout::parserHasTriggerEffects(parser);
    hw.hasPlayerLeds = input::usbout::parserHasPlayerLeds(parser);
    hw.hasMicLed = input::usbout::parserHasMicMuteLed(parser);
}

} // namespace

SlotRouter::SlotRouter(const Dependencies& deps) : deps_(deps) {}

SlotRouter::~SlotRouter() {
    table_.store(nullptr, std::memory_order_seq_cst);
    grace_.waitQuiescent();
}

void SlotRouter::attach() { deps_.processor->setSink(this); }

const SlotRouter::FeedbackRoute*
SlotRouter::RouteTable::feedbackFor(const QString& connectionId) const {
    for (const FeedbackRoute& route : feedback) {
        if (route.connectionId == connectionId) { return &route; }
    }
    return nullptr;
}

// A synthetic id packs its own (vid, pid), so the parser family IS the hardware truth for what the
// claim decodes: no device list is consulted.
SlotRouter::SlotHardware SlotRouter::syntheticHardware(int vendorId, int productId) {
    const auto parser = input::usbparse::parserForDevice(vendorId, productId);
    SlotHardware hw;
    hw.usbDirect = true;
    hw.hasMotion = input::usbparse::parserHasImu(parser);
    hw.hasTouchpad = input::usbparse::parserHasTouchpad(parser);
    hw.hasRumble = input::usbparse::parserHasRumble(parser);
    // The Direct path has an OUT report path, so a claimed pad answers for its real actuators.
    // Whether it can drive them right now is the router's call (the claim has to be live).
    hw.hasLightbar = input::usbout::parserHasLightbar(parser);
    applyOutputActuators(hw, parser);
    return hw;
}

// What SDL says about a pad it opened. An id SDL does not know answers all-false, which is the
// inert "no controller" state every caller already renders.
SlotRouter::SlotHardware SlotRouter::sdlHardware(const QString& slotId) const {
    SlotHardware hw;
    for (const auto& d : deps_.bridge->devices()) {
        if (d.id != slotId) { continue; }
        hw.hasMotion = d.motionCapable;
        hw.hasLightbar = d.hasLightbar;
        hw.hasTouchpad = d.hasTouchpad;
        hw.hasRumble = d.hasRumble;
        const auto parser = input::usbparse::parserForDevice(d.vendorId, d.productId);
        applyOutputActuators(hw, parser);
        // SDL_GameControllerSendEffect lands only in SDL's own HIDAPI drivers; XInput, DirectInput
        // and evdev refuse it. SDL names no driver, but it reports a DualSense's lightbar only from
        // that driver (the others have no LED call for it), so a DualSense with an LED is one whose
        // driver takes the effect body -- on USB or Bluetooth alike. The family gate matters: a
        // DualShock 4 has an LED from the same driver, but its effect body is a different report
        // with none of these surfaces.
        hw.sdlEffects = parser == input::usbparse::HidParser::DualSense && d.hasLightbar;
        return hw;
    }
    return hw;
}

SlotRouter::SlotHardware SlotRouter::slotHardware(const QString& slotId) const {
    // The synthetic id is checked first because it can never collide with an "sdl:N" id.
    const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
    if (vp.has_value() && reducer::isPadIdentity(vp)) {
        return syntheticHardware(vp->first, vp->second);
    }
    return sdlHardware(slotId);
}

reducer::SlotFeedbackInputs SlotRouter::feedbackInputs(const QString& slotId) const {
    const SlotHardware hw = slotHardware(slotId);
    reducer::SlotFeedbackInputs in;
    in.usbDirect = hw.usbDirect;
    in.padRumble = hw.hasRumble;
    in.padLightbar = hw.hasLightbar;
    in.padTriggerEffects = hw.hasTriggerEffects;
    in.padPlayerLeds = hw.hasPlayerLeds;
    in.padMicLed = hw.hasMicLed;
    in.standardEffects = hw.sdlEffects;
    if (hw.usbDirect) {
        const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
        in.directClaimLive = vp.has_value() && deps_.usbManager != nullptr &&
                             deps_.usbManager->isDirectClaimed(vp->first, vp->second);
    }
    // The matcher's live answer: whether THIS pad's own endpoints were confidently named on this
    // machine. Outside the Direct branch on purpose: the audio function is a separate USB interface
    // the OS keeps whichever path owns HID, so a Standard (SDL) pad on USB routes exactly like a
    // claimed one.
    const auto route = audioRouteForSlot(slotId);
    in.padMicRoute = route.microphone;
    in.padSpeakerRoute = route.speaker;
    in.padHapticRoute = route.haptics;
    return in;
}

audio::PadAudioRoute SlotRouter::audioRouteForSlot(const QString& slotId) const {
    // A synthetic id packs its own (vid, pid); an SDL slot's pad is looked up in the bridge, which
    // also says whether the link is Bluetooth. A BT pad never routes: it has no audio function,
    // and its VID:PID is the same as a USB twin's, so keying it would hand it that twin's endpoint.
    int vendorId = 0;
    int productId = 0;
    if (const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString()); vp.has_value()) {
        vendorId = vp->first;
        productId = vp->second;
    } else {
        bool found = false;
        for (const auto& d : deps_.bridge->devices()) {
            if (d.id != slotId) { continue; }
            if (d.bluetooth) { return {}; }
            vendorId = d.vendorId;
            productId = d.productId;
            found = true;
            break;
        }
        if (!found) { return {}; }
    }
    if (vendorId == 0 && productId == 0) { return {}; }
    std::lock_guard<thread::TrackedMutex> lock(audioRoutesMtx_);
    const auto it = padAudioRoutes_.find(audio::padAudioKey(vendorId, productId));
    if (it == padAudioRoutes_.end()) { return {}; }
    return it->second;
}

bool SlotRouter::setPadAudioRoutes(std::map<int, audio::PadAudioRoute> routes) {
    std::lock_guard<thread::TrackedMutex> lock(audioRoutesMtx_);
    if (routes == padAudioRoutes_) { return false; }
    padAudioRoutes_ = std::move(routes);
    return true;
}

SlotRouter::FeedbackRoute SlotRouter::liveRoute(const QString& slotId) const {
    FeedbackRoute route;
    route.slotId = slotId;
    const reducer::SlotFeedbackInputs in = feedbackInputs(slotId);
    for (const reducer::FeedbackKind kind : kFeedbackKindsInOrder) {
        route.targets[static_cast<std::size_t>(kind)] = reducer::resolveFeedbackTarget(in, kind);
    }
    if (in.usbDirect && deps_.usbManager != nullptr) {
        if (const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString())) {
            route.direct = deps_.usbManager->directTarget(vp->first, vp->second);
        }
    }
    route.padIndex = deps_.processor->find(slotId.toStdString());
    route.rumbleOn = deps_.rumbleEnabled->isEnabled(slotId);
    return route;
}

// Inputs from the slots on screen, feedback from every binding: a bound slot the list does not
// show (a twin hidden by its claim) still gets the rumble its session sends it.
std::unique_ptr<const SlotRouter::RouteTable> SlotRouter::build(const QStringList& slotIds) const {
    auto table = std::make_unique<RouteTable>();
    const auto bindings = deps_.hub->bindings();
    for (const QString& slotId : slotIds) {
        const std::string id = slotId.toStdString();
        const auto index = deps_.processor->add(id);
        const QString connectionId = bindings.value(slotId);
        net::WifiConnection* conn =
            connectionId.isEmpty() ? nullptr : deps_.wifi->get(connectionId);
        if (!index.has_value() || conn == nullptr) { continue; }
        auto target = conn->sendTarget();
        if (!target.has_value()) { continue; }
        InputRoute& route = table->inputs[*index];
        route.client = std::move(target->client);
        route.controllerIndex = target->controllerIndex;
        // The switch gates the motion stream itself, not only CAP_MOTION in the descriptor: a
        // satellite accepts MSG_MOTION without the cap.
        route.motion = deps_.motionEnabled->isEnabled(id);
    }
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        FeedbackRoute route = liveRoute(it.key());
        route.connectionId = it.value();
        table->feedback.push_back(std::move(route));
    }
    return table;
}

void SlotRouter::republish(const QStringList& slotIds) {
    std::unique_ptr<const RouteTable> next = build(slotIds);
    std::unique_ptr<const RouteTable> retired;
    {
        std::lock_guard<thread::TrackedMutex> lock(routesMtx_);
        table_.store(next.get(), std::memory_order_seq_cst);
        retired = std::exchange(published_, std::move(next));
        grace_.waitQuiescent();
    }
    // A Moonlight host's feedback follows the same changes: a claim, a pad, the rumble switch.
    if (anyMoonlight_.load(std::memory_order_relaxed)) { refreshMoonlightFeedback(); }
    // A client the old table held last is released here, outside the lock. Its session handed it
    // to the reaper when it let it go, and the reaper closes it, so the release joins nothing.
}

void SlotRouter::setMoonlightRoutes(const QString& slotId, MoonlightRoutes routes) {
    {
        std::lock_guard<thread::TrackedMutex> lock(moonlightMtx_);
        if (routes.report) {
            moonlightRoutes_[slotId.toStdString()] = std::move(routes);
        } else {
            moonlightRoutes_.erase(slotId.toStdString());
        }
        anyMoonlight_.store(!moonlightRoutes_.empty(), std::memory_order_relaxed);
    }
    refreshMoonlightFeedback();
}

void SlotRouter::refreshMoonlightFeedback() {
    std::vector<MoonlightFeedback> next;
    {
        std::lock_guard<thread::TrackedMutex> lock(moonlightMtx_);
        next.reserve(moonlightRoutes_.size());
        for (const auto& [slotId, routes] : moonlightRoutes_) {
            MoonlightFeedback feedback;
            feedback.hostUuid = routes.hostUuid;
            feedback.controllerNumber = routes.controllerNumber;
            feedback.route.slotId = QString::fromStdString(slotId);
            next.push_back(std::move(feedback));
        }
    }
    // Built with the lock released: a live route reads the bridge, the USB manager and the stores.
    for (MoonlightFeedback& feedback : next) { feedback.route = liveRoute(feedback.route.slotId); }
    std::lock_guard<thread::TrackedMutex> lock(moonlightMtx_);
    moonlightFeedback_.swap(next);
}

std::optional<SlotRouter::FeedbackRoute>
SlotRouter::moonlightFeedbackRoute(std::string_view hostUuid, std::uint8_t controllerNumber) const {
    if (!anyMoonlight_.load(std::memory_order_relaxed)) { return std::nullopt; }
    std::lock_guard<thread::TrackedMutex> lock(moonlightMtx_);
    for (const MoonlightFeedback& feedback : moonlightFeedback_) {
        if (feedback.controllerNumber == controllerNumber && feedback.hostUuid == hostUuid) {
            return feedback.route;
        }
    }
    return std::nullopt;
}

void SlotRouter::moonlightRumble(std::string_view hostUuid, std::uint8_t controllerNumber,
                                 std::uint16_t strong, std::uint16_t weak) {
    const auto route = moonlightFeedbackRoute(hostUuid, controllerNumber);
    if (!route.has_value()) { return; }
    // Duration 0: the host refreshes rumble on its own schedule and a stop is an explicit 0,0.
    const auto command = reducer::rumbleTheUserAllows({strong, weak, 0}, route->rumbleOn);
    routeRumble(*route, command.strong, command.weak, command.durationMs);
}

// The same gate as a satellite's MSG_LIGHTBAR: "Off" leaves the LED untouched.
void SlotRouter::moonlightLightbar(std::string_view hostUuid, std::uint8_t controllerNumber,
                                   std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const auto color = lightbarColorFromRgb(r, g, b, deps_.features->lightbarFollowGame());
    if (!color.has_value()) { return; }
    const auto route = moonlightFeedbackRoute(hostUuid, controllerNumber);
    if (!route.has_value()) { return; }
    routeLightbar(*route, color->r, color->g, color->b);
}

template <typename Sender>
Sender SlotRouter::moonlightSender(const DeviceId& id, Sender MoonlightRoutes::* member) const {
    if (!anyMoonlight_.load(std::memory_order_relaxed)) { return {}; }
    std::lock_guard<thread::TrackedMutex> lock(moonlightMtx_);
    const auto it = moonlightRoutes_.find(id);
    if (it == moonlightRoutes_.end()) { return {}; }
    return it->second.*member;
}

void SlotRouter::report(DeviceIndex index, const DeviceId& id, const DeviceState& s) {
    {
        const thread::GraceGuard guard(grace_, index);
        if (const RouteTable* table = table_.load(std::memory_order_seq_cst)) {
            const InputRoute& route = table->inputs[index];
            if (route.client) {
                route.client->sendReport(route.controllerIndex, s.wButtons, s.lt, s.rt, s.lx, s.ly,
                                         s.rx, s.ry);
                return;
            }
        }
    }
    // A slot bound to a Moonlight host has no session route and streams there instead, outside
    // the lock: the session takes its own.
    if (const auto send = moonlightSender(id, &MoonlightRoutes::report)) { send(s); }
}

void SlotRouter::motion(DeviceIndex index, const DeviceId& id, const MotionSample& sample,
                        std::uint32_t timestampDeltaUs) {
    {
        const thread::GraceGuard guard(grace_, index);
        if (const RouteTable* table = table_.load(std::memory_order_seq_cst)) {
            const InputRoute& route = table->inputs[index];
            if (route.client) {
                if (route.motion) {
                    route.client->sendMotion(route.controllerIndex, sample.gyroX, sample.gyroY,
                                             sample.gyroZ, sample.accelX, sample.accelY,
                                             sample.accelZ, timestampDeltaUs);
                }
                return;
            }
        }
    }
    if (const auto send = moonlightSender(id, &MoonlightRoutes::motion)) { send(sample); }
}

void SlotRouter::battery(DeviceIndex index, const DeviceId& id, const BatterySample& sample) {
    {
        const thread::GraceGuard guard(grace_, index);
        if (const RouteTable* table = table_.load(std::memory_order_seq_cst)) {
            const InputRoute& route = table->inputs[index];
            if (route.client) {
                route.client->sendBattery(route.controllerIndex, sample.level, sample.status);
                return;
            }
        }
    }
    // A Moonlight host that declared CAP_BATTERY is waiting for exactly this.
    if (const auto send = moonlightSender(id, &MoonlightRoutes::battery)) { send(sample); }
}

void SlotRouter::touchpad(DeviceIndex index, const DeviceId& id, const TouchpadSample& s) {
    {
        const thread::GraceGuard guard(grace_, index);
        if (const RouteTable* table = table_.load(std::memory_order_seq_cst)) {
            const InputRoute& route = table->inputs[index];
            if (route.client) {
                // Mouse-mode timing scales by the delta between uptime-ms stamps, so a frame keeps
                // the stamp its source gave it (a heal repeats one) and an unstamped one gets now.
                const std::uint32_t eventTimeMs =
                    s.eventTimeMs != 0
                        ? s.eventTimeMs
                        : static_cast<std::uint32_t>(
                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
                const auto fwd = reducer::assembleTouchpadForward(
                    s.finger0Active, s.finger0Id, s.finger0X, s.finger0Y, s.finger1Active,
                    s.finger1Id, s.finger1X, s.finger1Y, s.buttonPressed, eventTimeMs);
                route.client->sendTouchpad(route.controllerIndex, fwd.finger0Active, fwd.finger0Id,
                                           fwd.finger0X, fwd.finger0Y, fwd.finger1Active,
                                           fwd.finger1Id, fwd.finger1X, fwd.finger1Y,
                                           fwd.buttonPressed, fwd.eventTimeMs);
                return;
            }
        }
    }
    // A Moonlight-bound slot takes the SAME full-state frame; its sender diffs it into
    // CONTROLLER_TOUCH events on the way out.
    if (const auto send = moonlightSender(id, &MoonlightRoutes::touch)) { send(s); }
}

void SlotRouter::sendBattery(const QString& slotId, std::uint8_t level, std::uint8_t status) {
    const std::string id = slotId.toStdString();
    {
        std::lock_guard<thread::TrackedMutex> lock(routesMtx_);
        const auto index = deps_.processor->add(id);
        if (published_ != nullptr && index.has_value()) {
            const InputRoute& route = published_->inputs[*index];
            if (route.client) {
                route.client->sendBattery(route.controllerIndex, level, status);
                return;
            }
        }
    }
    if (const auto send = moonlightSender(id, &MoonlightRoutes::battery)) {
        send(BatterySample{level, status});
    }
}

template <typename Deliver>
void SlotRouter::withFeedbackRoute(std::size_t participant, const QString& connectionId,
                                   Deliver&& deliver) {
    const thread::GraceGuard guard(grace_, participant);
    const RouteTable* table = table_.load(std::memory_order_seq_cst);
    if (table == nullptr) { return; }
    if (const FeedbackRoute* route = table->feedbackFor(connectionId)) { deliver(*route); }
}

// Every one of these runs on the session's receive thread. The lambdas carry no algorithm: they
// bind the connection id, which the message itself does not carry, and the session's participant.
void SlotRouter::installFeedbackHandlers(net::WifiConnection& conn, const QString& connectionId) {
    std::size_t participant = 0;
    if (const auto known = sessionParticipants_.constFind(connectionId);
        known != sessionParticipants_.constEnd()) {
        participant = *known;
    } else {
        if (static_cast<std::size_t>(sessionParticipants_.size()) >= kSessionParticipants) {
            return;
        }
        participant = kDeviceParticipants + static_cast<std::size_t>(sessionParticipants_.size());
        sessionParticipants_.insert(connectionId, participant);
    }
    conn.setRumbleHandler(
        [this, participant, connectionId](const net::SatelliteClient::RumbleMessage& rm) {
            withFeedbackRoute(participant, connectionId, [&](const FeedbackRoute& route) {
                const auto command = reducer::rumbleTheUserAllows(
                    {rm.strongMagnitude, rm.weakMagnitude, rm.durationMs}, route.rumbleOn);
                routeRumble(route, command.strong, command.weak, command.durationMs);
            });
        });
    conn.setLightbarHandler(
        [this, participant, connectionId](const net::SatelliteClient::LightbarMessage& lm) {
            // Gated by the light-bar setting: "Off" suppresses the colour entirely.
            const auto color =
                lightbarColorFromLightbarMessage(lm, deps_.features->lightbarFollowGame());
            if (!color) { return; }
            withFeedbackRoute(participant, connectionId, [&](const FeedbackRoute& route) {
                routeLightbar(route, color->r, color->g, color->b);
            });
        });
    conn.setTriggerEffectsHandler(
        [this, participant, connectionId](const net::SatelliteClient::TriggerEffectsMessage& tm) {
            withFeedbackRoute(participant, connectionId, [&](const FeedbackRoute& route) {
                routeTriggerEffects(route, tm.left, tm.right);
            });
        });
    // The indicator bar. Deliberately NOT gated on the light-bar setting: that switch is about the
    // RGB colour following the game, and a player number is not a colour.
    conn.setPlayerLedsHandler([this, participant,
                               connectionId](const net::SatelliteClient::PlayerLedsMessage& pm) {
        withFeedbackRoute(participant, connectionId,
                          [&](const FeedbackRoute& route) { routePlayerLeds(route, pm.ledMask); });
    });
    conn.setMicLedHandler(
        [this, participant, connectionId](const net::SatelliteClient::MicLedMessage& mm) {
            withFeedbackRoute(participant, connectionId,
                              [&](const FeedbackRoute& route) { routeMicLed(route, mm.state); });
        });
    // Handed to the playout engine, which copies the frame onto its own thread and returns at
    // once, so a decode burst cannot delay the heartbeat ack behind it. The message borrows the
    // receive buffer; the engine copies it before returning.
    const auto engineId = std::make_shared<const std::string>(connectionId.toStdString());
    conn.setSpeakerAudioHandler([this,
                                 engineId](const net::SatelliteClient::SpeakerAudioMessage& sm) {
        deps_.speaker->deliver(*engineId, sm.controllerIndex, source::audio::PlayoutLane::Speaker,
                               sm.seq, sm.opus, sm.opusLen);
    });
    conn.setHapticAudioHandler(
        [this, engineId](const net::SatelliteClient::SpeakerAudioMessage& hm) {
            deps_.speaker->deliver(*engineId, hm.controllerIndex,
                                   source::audio::PlayoutLane::Haptic, hm.seq, hm.opus, hm.opusLen);
        });
}

void SlotRouter::routeRumble(const FeedbackRoute& route, std::uint16_t strong, std::uint16_t weak,
                             std::uint16_t durationMs) {
    switch (route.target(reducer::FeedbackKind::Rumble)) {
    case reducer::FeedbackTarget::Standard:
        // The SDL thread actuates it from the pad's mailbox on its next cycle.
        if (route.padIndex.has_value()) {
            deps_.bridge->postRumble(*route.padIndex, strong, weak, durationMs);
            rumblesRouted_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    case reducer::FeedbackTarget::DirectUsb:
        // No duration on the raw path: the pad's motors run until the next write, and the
        // satellite refreshes well before its own expiry. A stop is magnitudes 0,0.
        if (route.direct.has_value() && deps_.usbManager != nullptr) {
            deps_.usbManager->applyRumble(*route.direct, strong, weak);
            rumblesRouted_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    case reducer::FeedbackTarget::None:
        return;
    }
}

void SlotRouter::routeLightbar(const FeedbackRoute& route, std::uint8_t r, std::uint8_t g,
                               std::uint8_t b) {
    switch (route.target(reducer::FeedbackKind::Lightbar)) {
    case reducer::FeedbackTarget::Standard:
        if (route.padIndex.has_value()) {
            deps_.bridge->postLightbar(*route.padIndex, r, g, b);
            lightbarsRouted_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    case reducer::FeedbackTarget::DirectUsb:
        if (route.direct.has_value() && deps_.usbManager != nullptr) {
            deps_.usbManager->applyLightbar(*route.direct, r, g, b);
            lightbarsRouted_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    case reducer::FeedbackTarget::None:
        return;
    }
}

void SlotRouter::routeTriggerEffects(
    const FeedbackRoute& route,
    const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& left,
    const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& right) {
    switch (route.target(reducer::FeedbackKind::TriggerEffects)) {
    case reducer::FeedbackTarget::Standard:
        // The same report the Direct branch writes, built on the SDL thread and handed to SDL's
        // driver instead of the device.
        if (route.padIndex.has_value()) {
            deps_.bridge->postTriggerEffects(*route.padIndex, left, right);
        }
        return;
    case reducer::FeedbackTarget::DirectUsb:
        if (route.direct.has_value() && deps_.usbManager != nullptr) {
            deps_.usbManager->applyTriggerEffects(*route.direct, left.data(), right.data());
        }
        return;
    case reducer::FeedbackTarget::None:
        return;
    }
}

void SlotRouter::routePlayerLeds(const FeedbackRoute& route, std::uint8_t ledMask) {
    switch (route.target(reducer::FeedbackKind::PlayerLeds)) {
    case reducer::FeedbackTarget::Standard:
        if (route.padIndex.has_value()) { deps_.bridge->postPlayerLeds(*route.padIndex, ledMask); }
        return;
    case reducer::FeedbackTarget::DirectUsb:
        if (route.direct.has_value() && deps_.usbManager != nullptr) {
            deps_.usbManager->applyPlayerLeds(*route.direct, ledMask);
        }
        return;
    case reducer::FeedbackTarget::None:
        return;
    }
}

void SlotRouter::routeMicLed(const FeedbackRoute& route, std::uint8_t state) {
    switch (route.target(reducer::FeedbackKind::MicLed)) {
    case reducer::FeedbackTarget::Standard:
        if (route.padIndex.has_value()) { deps_.bridge->postMicLed(*route.padIndex, state); }
        return;
    case reducer::FeedbackTarget::DirectUsb:
        if (route.direct.has_value() && deps_.usbManager != nullptr) {
            deps_.usbManager->applyMicMuteLed(*route.direct, state);
        }
        return;
    case reducer::FeedbackTarget::None:
        return;
    }
}

void SlotRouter::deliverRumble(const QString& slotId, std::uint16_t strong, std::uint16_t weak,
                               std::uint16_t durationMs) {
    const FeedbackRoute route = liveRoute(slotId);
    const auto command = reducer::rumbleTheUserAllows({strong, weak, durationMs}, route.rumbleOn);
    routeRumble(route, command.strong, command.weak, command.durationMs);
}

void SlotRouter::actuateRumble(const QString& slotId, std::uint16_t strong, std::uint16_t weak,
                               std::uint16_t durationMs) {
    routeRumble(liveRoute(slotId), strong, weak, durationMs);
}

void SlotRouter::actuateLightbar(const QString& slotId, std::uint8_t r, std::uint8_t g,
                                 std::uint8_t b) {
    routeLightbar(liveRoute(slotId), r, g, b);
}

void SlotRouter::actuateTriggerEffects(
    const QString& slotId, const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& left,
    const std::array<std::uint8_t, proto::kTriggerEffectBlockBytes>& right) {
    routeTriggerEffects(liveRoute(slotId), left, right);
}

void SlotRouter::actuatePlayerLeds(const QString& slotId, std::uint8_t ledMask) {
    routePlayerLeds(liveRoute(slotId), ledMask);
}

void SlotRouter::actuateMicLed(const QString& slotId, std::uint8_t state) {
    routeMicLed(liveRoute(slotId), state);
}

} // namespace dish
