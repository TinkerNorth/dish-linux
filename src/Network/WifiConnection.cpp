// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "WifiConnection.h"

#include "core/reducer/Reconcile.h"

#include <QSignalBlocker>

#include <cmath>

namespace dish::net {

namespace {

// What every descriptor advertises. CAP_RUMBLE, CAP_MOTION, CAP_LIGHTBAR and
// the protocol-2 CAP_TRIGGER_EFFECTS / CAP_PLAYER_LEDS are folded in
// per-controller instead: motion from the slot's own hardware, and every
// actuator from the path that would carry it (core/reducer/FeedbackRouting.h).
constexpr std::uint16_t kBaseCaps = SatelliteClient::kCapAnalogTriggers;

} // namespace

WifiConnection::WifiConnection(QString id, models::DiscoveredServer server, QObject* parent)
    : QObject(parent), id_(std::move(id)), server_(std::move(server)) {}

WifiConnection::~WifiConnection() {
    // The teardown is what matters here; the announcement never does. Emitting
    // `changed` from a destructor invites every listener to call straight back
    // into an object that is already half-gone, and at app exit their own
    // collaborators may be gone too: ~WifiConnectionManager runs from
    // ~QObject's deleteChildren, i.e. AFTER every AppModel member — including
    // the ConnectionStore that ConnectionHub::rebuild() reads through when it
    // hears poolChanged. That was an access violation on every exit.
    //
    // The manager's own teardown loop blocks the signal case by case, but it
    // only sees the connections still in its map: a forget() has already
    // `take`n its connection out and left it on deleteLater, and any future
    // caller could do the same. Blocking here covers the whole class, at the
    // one place that knows the object is dying.
    const QSignalBlocker block(this);
    markDisconnected();
}

void WifiConnection::updateServer(const models::DiscoveredServer& s) {
    server_ = s;
    emit changed();
}

void WifiConnection::markConnecting() {
    if (state_ == SessionState::Live) { return; }
    state_ = SessionState::Linking;
    emit changed();
}

models::ControllerDescriptor WifiConnection::descriptorOf(const SlotBinding& b) const {
    models::ControllerDescriptor d;
    d.ctrlIdx = b.controllerIndex;
    d.type = static_cast<std::uint8_t>(b.controllerType);
    std::uint16_t caps = kBaseCaps;
    caps = SatelliteClient::withRumbleCapability(caps, b.hasRumble);
    caps = SatelliteClient::withMotionCapability(caps, b.hasMotion);
    caps = SatelliteClient::withLightbarCapability(caps, b.hasLightbar);
    caps = SatelliteClient::withTriggerEffectsCapability(caps, b.hasTriggerEffects);
    caps = SatelliteClient::withPlayerLedsCapability(caps, b.hasPlayerLeds);
    caps = SatelliteClient::withMicCapability(caps, b.hasMic);
    caps = SatelliteClient::withSpeakerCapability(caps, b.hasSpeaker);
    d.caps = caps;
    d.touchpadMode = b.touchpadMode;
    return d;
}

int WifiConnection::lowestFreeIndex() const {
    int i = 0;
    for (;;) {
        bool taken = false;
        for (const auto& [slotId, b] : slots_) {
            if (b.controllerIndex == i) {
                taken = true;
                break;
            }
        }
        if (!taken) { return i; }
        ++i;
    }
}

void WifiConnection::teardownClient() {
    if (aliveTimer_ != nullptr) {
        aliveTimer_->stop();
        aliveTimer_->deleteLater();
        aliveTimer_ = nullptr;
    }
    if (auto existing = clientRef_.get()) {
        existing->stopHeartbeat();
        existing->stopReceiveLoop();
        existing->closeSocket();
    }
    clientRef_.set(nullptr);
    connectionId_.reset();
    lastAppliedEpoch_ = -1;
    mouseControlGranted_ = false;
    // Session-scoped, so a reconnect must not show the old figure. Every teardown
    // path emits changed() right after, so no telemetryChanged is needed.
    latencyOneWayMs_ = 0.0;
    latencySamples_ = 0;
    rekeyRequested_ = false;
    // Session state, so the next session starts from the conservative "no
    // audio" answer and waits for its own probe.
    hostMic_ = false;
    hostSpeaker_ = false;
    // A dropped session leaves no virtual pads applied, so streams must gate off
    // until the next PUT re-applies them.
    for (auto& [slotId, b] : slots_) { b.registered = false; }
}

void WifiConnection::markConnected(const std::shared_ptr<SatelliteClient>& client,
                                   const QString& connectionId, int epoch, bool mouseControlGranted,
                                   std::function<void()> onDead,
                                   std::function<void(std::uint8_t)> onClose,
                                   std::function<void()> onReconcile,
                                   std::function<void()> onRekey) {
    if (state_ != SessionState::Linking) { return; }
    clientRef_.set(client);
    connectionId_ = connectionId;
    lastAppliedEpoch_ = epoch;
    mouseControlGranted_ = mouseControlGranted;
    state_ = SessionState::Live;
    onDead_ = std::move(onDead);
    onClose_ = std::move(onClose);
    onReconcile_ = std::move(onReconcile);
    onRekey_ = std::move(onRekey);
    rekeyRequested_ = false;

    if (rumbleHandler_) { client->setRumbleHandler(rumbleHandler_); }
    if (lightbarHandler_) { client->setLightbarHandler(lightbarHandler_); }
    if (triggerEffectsHandler_) { client->setTriggerEffectsHandler(triggerEffectsHandler_); }
    if (playerLedsHandler_) { client->setPlayerLedsHandler(playerLedsHandler_); }
    if (speakerAudioHandler_) { client->setSpeakerAudioHandler(speakerAudioHandler_); }
    if (micLedHandler_) { client->setMicLedHandler(micLedHandler_); }
    // Rumble and lightbar may fire on the receive thread because they only hand
    // off to the SDL bridge's own queue. Close-notify and the ack reconcile drive
    // the session FSM and REST, so they are polled by the main-thread alive timer
    // off the client's atomics rather than pushed from that thread.
    client->startReceiveLoop();
    client->startHeartbeat();

    if (aliveTimer_ != nullptr) {
        aliveTimer_->stop();
        aliveTimer_->deleteLater();
    }
    aliveTimer_ = new QTimer(this);
    aliveTimer_->setInterval(1000);
    QObject::connect(aliveTimer_, &QTimer::timeout, this, &WifiConnection::onAliveTick);
    aliveTimer_->start();
    emit changed();
}

void WifiConnection::onAliveTick() {
    const auto c = clientRef_.get();
    if (!c) { return; }
    // Rounded to the display precision so sub-jitter median moves do not re-emit.
    const auto latency = c->latencySnapshot();
    const double rounded = latency.samples > 0
                               ? static_cast<double>(std::lround(latency.oneWayMs * 10.0)) / 10.0
                               : 0.0;
    if (rounded != latencyOneWayMs_ || latency.samples != latencySamples_) {
        latencyOneWayMs_ = rounded;
        latencySamples_ = latency.samples;
        emit telemetryChanged();
    }
    // Terminal immediately: the session is already gone server-side, so there is
    // nothing to wait out.
    const std::int32_t closeReason = c->sessionCloseReason();
    if (closeReason >= 0) {
        const auto cb = onClose_;
        if (cb) { cb(static_cast<std::uint8_t>(closeReason)); }
        return;
    }
    if (!c->isAlive()) {
        const auto cb = onDead_;
        if (cb) { cb(); }
        return;
    }
    // Flips only between the two steady states; Linking and Stale keep their own
    // owners. Recovers to Live the moment an ack lands.
    const bool faltering = c->missedAcks() >= SatelliteClient::kHeartbeatMissNotResponding;
    const SessionState steady = faltering ? SessionState::Faltering : SessionState::Live;
    if ((state_ == SessionState::Live || state_ == SessionState::Faltering) && state_ != steady) {
        state_ = steady;
        emit changed();
    }
    // A nudge only: the manager does the GET-then-rePUT solely when epoch or
    // bitmap actually diverged.
    const auto cb = onReconcile_;
    if (cb) { cb(); }
    // Re-key ahead of counter exhaustion. A session that exhausts anyway goes
    // silent in SatelliteClient and heals via the death-retry re-PUT.
    if (reducer::counterNeedsRepush(c->sendCounter())) {
        if (!rekeyRequested_ && onRekey_) {
            rekeyRequested_ = true;
            onRekey_();
        }
    } else {
        rekeyRequested_ = false;
    }
}

void WifiConnection::markDisconnected() {
    auto existing = clientRef_.get();
    if (state_ == SessionState::Idle && !existing) { return; }
    teardownClient();
    state_ = SessionState::Idle;
    emit changed();
}

void WifiConnection::markStale() {
    auto existing = clientRef_.get();
    if (state_ == SessionState::Stale && !existing) { return; }
    teardownClient();
    state_ = SessionState::Stale;
    emit changed();
}

void WifiConnection::attachSlot(const QString& slotId, int controllerType, bool hasLightbar,
                                bool hasMotion, bool hasRumble, std::uint8_t touchpadMode,
                                bool hasTriggerEffects, bool hasPlayerLeds, bool hasMic,
                                bool hasSpeaker) {
    auto it = slots_.find(slotId);
    if (it == slots_.end()) {
        SlotBinding b;
        b.controllerIndex = lowestFreeIndex();
        b.controllerType = controllerType;
        b.hasLightbar = hasLightbar;
        b.hasMotion = hasMotion;
        b.hasRumble = hasRumble;
        b.hasTriggerEffects = hasTriggerEffects;
        b.hasPlayerLeds = hasPlayerLeds;
        b.hasMic = hasMic;
        b.hasSpeaker = hasSpeaker;
        b.touchpadMode = touchpadMode;
        b.registered = false;
        slots_.emplace(slotId, b);
        boundSlotId_ = slotId;
        if (state_ == SessionState::Live) { emit slotChanged(slotId); }
    } else {
        const bool changed =
            it->second.controllerType != controllerType || it->second.hasLightbar != hasLightbar ||
            it->second.hasMotion != hasMotion || it->second.hasRumble != hasRumble ||
            it->second.touchpadMode != touchpadMode ||
            it->second.hasTriggerEffects != hasTriggerEffects ||
            it->second.hasPlayerLeds != hasPlayerLeds || it->second.hasMic != hasMic ||
            it->second.hasSpeaker != hasSpeaker;
        it->second.controllerType = controllerType;
        it->second.hasLightbar = hasLightbar;
        it->second.hasMotion = hasMotion;
        it->second.hasRumble = hasRumble;
        it->second.hasTriggerEffects = hasTriggerEffects;
        it->second.hasPlayerLeds = hasPlayerLeds;
        it->second.hasMic = hasMic;
        it->second.hasSpeaker = hasSpeaker;
        it->second.touchpadMode = touchpadMode;
        if (changed && state_ == SessionState::Live) { emit slotChanged(slotId); }
    }
    emit changed();
}

void WifiConnection::detachSlot() {
    if (!boundSlotId_.has_value()) { return; }
    detachSlot(*boundSlotId_);
}

void WifiConnection::detachSlot(const QString& slotId) {
    const auto it = slots_.find(slotId);
    if (it == slots_.end()) { return; }
    const SlotBinding removed = it->second;
    slots_.erase(it);
    if (boundSlotId_.has_value() && *boundSlotId_ == slotId) { boundSlotId_.reset(); }
    // The session itself lives on; a zero-controller session is valid.
    if (state_ == SessionState::Live && removed.registered) {
        emit slotRemoved(removed.controllerIndex);
    }
    emit changed();
}

std::optional<models::ControllerDescriptor>
WifiConnection::descriptorFor(const QString& slotId) const {
    const auto it = slots_.find(slotId);
    if (it == slots_.end()) { return std::nullopt; }
    return descriptorOf(it->second);
}

QList<models::ControllerDescriptor> WifiConnection::desiredDescriptors() const {
    QList<models::ControllerDescriptor> out;
    for (const auto& [slotId, b] : slots_) { out.append(descriptorOf(b)); }
    return out;
}

QString WifiConnection::slotIdForIndex(int ctrlIdx) const {
    for (const auto& [slotId, b] : slots_) {
        if (b.controllerIndex == ctrlIdx) { return slotId; }
    }
    return {};
}

bool WifiConnection::wantsMouseControl() const {
    for (const auto& [slotId, b] : slots_) {
        if (b.touchpadMode == proto::kTouchpadModeMouse) { return true; }
    }
    return false;
}

void WifiConnection::applyResults(const QList<models::ControllerApplyDto>& results) {
    if (results.isEmpty()) { return; }
    std::map<int, const models::ControllerApplyDto*> byIdx;
    for (const auto& r : results) { byIdx[r.ctrlIdx] = &r; }
    QList<models::ControllerApplyDto> failures;
    for (auto& [slotId, b] : slots_) {
        const auto it = byIdx.find(b.controllerIndex);
        if (it == byIdx.end()) { continue; }
        const auto& r = *it->second;
        // A failed replug keeps the previous pad alive, so streams keep flowing
        // rather than killing a working pad over a type the driver could not
        // switch to.
        b.registered = r.slotIsLive();
        if (!r.ok()) { failures.append(r); }
    }
    emit changed();
    for (const auto& f : failures) {
        emit errorOccurred(QStringLiteral("Controller #%1: %2").arg(f.ctrlIdx).arg(f.result));
    }
}

std::uint16_t WifiConnection::registeredBitmap() const {
    std::vector<reducer::DesiredSlot> reg;
    for (const auto& [slotId, b] : slots_) {
        if (b.registered) {
            reg.push_back({static_cast<std::uint8_t>(b.controllerIndex),
                           static_cast<std::uint8_t>(b.controllerType)});
        }
    }
    return reducer::expectedBitmap(reg);
}

bool WifiConnection::matchesAppliedView(const models::SessionViewDto& view) const {
    std::vector<reducer::DesiredSlot> desired;
    desired.reserve(slots_.size());
    for (const auto& [slotId, b] : slots_) {
        desired.push_back({static_cast<std::uint8_t>(b.controllerIndex),
                           static_cast<std::uint8_t>(b.controllerType), b.touchpadMode});
    }
    std::vector<reducer::AppliedSlot> applied;
    for (const auto& c : view.controllers) {
        reducer::AppliedSlot a{static_cast<std::uint8_t>(c.ctrlIdx),
                               static_cast<std::uint8_t>(c.appliedType), c.active, std::nullopt};
        // An empty mode string means the server does not report it; skip the mode
        // arm rather than forcing a pointless re-PUT.
        if (!c.touchpadMode.isEmpty()) {
            a.touchpadMode = proto::touchpadModeFromName(c.touchpadMode.toStdString());
        }
        applied.push_back(a);
    }
    // The grant is applied state too. Since the server computes it only at
    // session PUT, a slot toggled to mouse mid-session leaves wants != granted,
    // and the converge re-PUT is what heals it.
    const bool mouseMatch = view.mouseControl.granted == wantsMouseControl();
    return reducer::appliedMatchesDesired(desired, applied, mouseMatch);
}

void WifiConnection::sendReport(std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt,
                                std::int16_t lx, std::int16_t ly, std::int16_t rx,
                                std::int16_t ry) {
    if (!boundSlotId_.has_value()) { return; }
    const auto it = slots_.find(*boundSlotId_);
    if (it == slots_.end() || !it->second.registered) { return; }
    if (auto c = clientRef_.get()) {
        c->sendReport(it->second.controllerIndex, buttons, lt, rt, lx, ly, rx, ry);
    }
}

void WifiConnection::sendMotion(std::int16_t gyroX, std::int16_t gyroY, std::int16_t gyroZ,
                                std::int16_t accelX, std::int16_t accelY, std::int16_t accelZ,
                                std::uint32_t timestampDeltaUs) {
    if (!boundSlotId_.has_value()) { return; }
    const auto it = slots_.find(*boundSlotId_);
    if (it == slots_.end() || !it->second.registered) { return; }
    if (auto c = clientRef_.get()) {
        c->sendMotion(it->second.controllerIndex, gyroX, gyroY, gyroZ, accelX, accelY, accelZ,
                      timestampDeltaUs);
    }
}

void WifiConnection::sendBattery(std::uint8_t level, std::uint8_t status) {
    if (!boundSlotId_.has_value()) { return; }
    const auto it = slots_.find(*boundSlotId_);
    if (it == slots_.end() || !it->second.registered) { return; }
    if (auto c = clientRef_.get()) { c->sendBattery(it->second.controllerIndex, level, status); }
}

void WifiConnection::sendTouchpad(bool finger0Active, std::uint8_t finger0Id, std::int16_t finger0X,
                                  std::int16_t finger0Y, bool finger1Active, std::uint8_t finger1Id,
                                  std::int16_t finger1X, std::int16_t finger1Y, bool buttonPressed,
                                  std::uint32_t eventTimeMs) {
    if (!boundSlotId_.has_value()) { return; }
    const auto it = slots_.find(*boundSlotId_);
    if (it == slots_.end() || !it->second.registered) { return; }
    if (auto c = clientRef_.get()) {
        c->sendTouchpad(it->second.controllerIndex, finger0Active, finger0Id, finger0X, finger0Y,
                        finger1Active, finger1Id, finger1X, finger1Y, buttonPressed, eventTimeMs);
    }
}

bool WifiConnection::sendMicAudio(std::uint16_t seq, const std::uint8_t* opus,
                                  std::size_t opusLen) {
    if (!boundSlotId_.has_value()) { return false; }
    const auto it = slots_.find(*boundSlotId_);
    if (it == slots_.end() || !it->second.registered) { return false; }
    if (auto c = clientRef_.get()) {
        return c->sendMicAudio(it->second.controllerIndex, seq, opus, opusLen);
    }
    return false;
}

void WifiConnection::setRumbleHandler(RumbleHandler handler) {
    rumbleHandler_ = std::move(handler);
    if (auto c = clientRef_.get()) { c->setRumbleHandler(rumbleHandler_); }
}

void WifiConnection::setLightbarHandler(LightbarHandler handler) {
    lightbarHandler_ = std::move(handler);
    if (auto c = clientRef_.get()) { c->setLightbarHandler(lightbarHandler_); }
}

void WifiConnection::setTriggerEffectsHandler(TriggerEffectsHandler handler) {
    triggerEffectsHandler_ = std::move(handler);
    if (auto c = clientRef_.get()) { c->setTriggerEffectsHandler(triggerEffectsHandler_); }
}

void WifiConnection::setPlayerLedsHandler(PlayerLedsHandler handler) {
    playerLedsHandler_ = std::move(handler);
    if (auto c = clientRef_.get()) { c->setPlayerLedsHandler(playerLedsHandler_); }
}

void WifiConnection::setSpeakerAudioHandler(SpeakerAudioHandler handler) {
    speakerAudioHandler_ = std::move(handler);
    if (auto c = clientRef_.get()) { c->setSpeakerAudioHandler(speakerAudioHandler_); }
}

void WifiConnection::setMicLedHandler(MicLedHandler handler) {
    micLedHandler_ = std::move(handler);
    if (auto c = clientRef_.get()) { c->setMicLedHandler(micLedHandler_); }
}

void WifiConnection::setHostControllerAudio(bool mic, bool speaker) {
    if (hostMic_ == mic && hostSpeaker_ == speaker) { return; }
    hostMic_ = mic;
    hostSpeaker_ = speaker;
    // The capability table's host layer reads it, so a landed probe must
    // re-render the rows.
    emit changed();
}

} // namespace dish::net
