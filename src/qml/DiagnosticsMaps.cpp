// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "qml/DiagnosticsMaps.h"

#include "Network/WifiConnection.h"
#include "core/input/InputReadout.h"
#include "core/model/Protocol.h"
#include "qml/AppSettingsMaps.h"
#include "qml/RenderTokens.h"

#include <array>
#include <string_view>

namespace dish::qml {

namespace {

// What SatelliteClient's ack fields read before the first enriched ack, and so
// what a connection with no client reads too.
constexpr int kNotAcked = -1;

QString agreementToken(reducer::Agreement agreement) {
    switch (agreement) {
    case reducer::Agreement::Unknown:
        return QStringLiteral("unknown");
    case reducer::Agreement::InStep:
        return QStringLiteral("inStep");
    case reducer::Agreement::Diverged:
        return QStringLiteral("diverged");
    }
    return {};
}

QString streamingToken(reducer::Streaming streaming) {
    switch (streaming) {
    case reducer::Streaming::Unknown:
        return QStringLiteral("unknown");
    case reducer::Streaming::Yes:
        return QStringLiteral("yes");
    case reducer::Streaming::No:
        return QStringLiteral("no");
    }
    return {};
}

QString hostBackendToken(reducer::HostBackend backend) {
    switch (backend) {
    case reducer::HostBackend::Unknown:
        return QStringLiteral("unknown");
    case reducer::HostBackend::Available:
        return QStringLiteral("available");
    case reducer::HostBackend::Unavailable:
        return QStringLiteral("unavailable");
    }
    return {};
}

// The capability table's feature vocabulary, so a page names an advertised cap
// with the same word it names the capability row.
QString wireCapToken(reducer::WireCap cap) {
    switch (cap) {
    case reducer::WireCap::AnalogTriggers:
        return QStringLiteral("triggers");
    case reducer::WireCap::Rumble:
        return QStringLiteral("rumble");
    case reducer::WireCap::Motion:
        return QStringLiteral("motion");
    case reducer::WireCap::Lightbar:
        return QStringLiteral("lightbar");
    case reducer::WireCap::TriggerEffects:
        return QStringLiteral("triggerEffects");
    case reducer::WireCap::PlayerLeds:
        return QStringLiteral("playerLeds");
    case reducer::WireCap::Mic:
        return QStringLiteral("mic");
    case reducer::WireCap::Speaker:
        return QStringLiteral("speaker");
    case reducer::WireCap::HapticAudio:
        return QStringLiteral("hapticAudio");
    }
    return {};
}

QString wireButtonToken(input::WireButton button) {
    switch (button) {
    case input::WireButton::DpadUp:
        return QStringLiteral("dpadUp");
    case input::WireButton::DpadDown:
        return QStringLiteral("dpadDown");
    case input::WireButton::DpadLeft:
        return QStringLiteral("dpadLeft");
    case input::WireButton::DpadRight:
        return QStringLiteral("dpadRight");
    case input::WireButton::Start:
        return QStringLiteral("start");
    case input::WireButton::Back:
        return QStringLiteral("back");
    case input::WireButton::LeftThumb:
        return QStringLiteral("leftThumb");
    case input::WireButton::RightThumb:
        return QStringLiteral("rightThumb");
    case input::WireButton::LeftShoulder:
        return QStringLiteral("leftShoulder");
    case input::WireButton::RightShoulder:
        return QStringLiteral("rightShoulder");
    case input::WireButton::Guide:
        return QStringLiteral("guide");
    case input::WireButton::MicMute:
        return QStringLiteral("micMute");
    case input::WireButton::A:
        return QStringLiteral("a");
    case input::WireButton::B:
        return QStringLiteral("b");
    case input::WireButton::X:
        return QStringLiteral("x");
    case input::WireButton::Y:
        return QStringLiteral("y");
    }
    return {};
}

QString eventKindToken(reducer::DiagnosticsEventKind kind) {
    switch (kind) {
    case reducer::DiagnosticsEventKind::LinkAppeared:
        return QStringLiteral("linkAppeared");
    case reducer::DiagnosticsEventKind::LinkChanged:
        return QStringLiteral("linkChanged");
    case reducer::DiagnosticsEventKind::LinkRemoved:
        return QStringLiteral("linkRemoved");
    case reducer::DiagnosticsEventKind::PadAttached:
        return QStringLiteral("padAttached");
    case reducer::DiagnosticsEventKind::PadDetached:
        return QStringLiteral("padDetached");
    case reducer::DiagnosticsEventKind::PadNeedsReplug:
        return QStringLiteral("padNeedsReplug");
    case reducer::DiagnosticsEventKind::PadRestoreStuck:
        return QStringLiteral("padRestoreStuck");
    case reducer::DiagnosticsEventKind::PadDirectFailed:
        return QStringLiteral("padDirectFailed");
    case reducer::DiagnosticsEventKind::PadBound:
        return QStringLiteral("padBound");
    case reducer::DiagnosticsEventKind::PadUnbound:
        return QStringLiteral("padUnbound");
    }
    return {};
}

QString padPathToken(reducer::PadPath path) {
    switch (path) {
    case reducer::PadPath::UsbStandard:
        return QStringLiteral("usbStandard");
    case reducer::PadPath::UsbDirect:
        return QStringLiteral("usbDirect");
    case reducer::PadPath::Bluetooth:
        return QStringLiteral("bluetooth");
    }
    return {};
}

QString stickTestPhaseToken(input::StickTestPhase phase) {
    switch (phase) {
    case input::StickTestPhase::Idle:
        return QStringLiteral("idle");
    case input::StickTestPhase::Running:
        return QStringLiteral("running");
    case input::StickTestPhase::Done:
        return QStringLiteral("done");
    }
    return {};
}

constexpr std::array<input::StickTestKind, 2> kStickTestKinds{input::StickTestKind::Drift,
                                                              input::StickTestKind::Range};

struct BuzzMotorWord {
    reducer::BuzzMotor motor = reducer::BuzzMotor::Weak;
    const char* token = "";
};

constexpr std::array<BuzzMotorWord, 3> kBuzzMotorWords{{
    {reducer::BuzzMotor::Weak, "weak"},
    {reducer::BuzzMotor::Strong, "strong"},
    {reducer::BuzzMotor::Both, "both"},
}};

QString stickTestKindToken(input::StickTestKind kind) {
    switch (kind) {
    case input::StickTestKind::Drift:
        return QStringLiteral("drift");
    case input::StickTestKind::Range:
        return QStringLiteral("range");
    }
    return {};
}

// A link's state reads as the chip the Connections page wears for it, so the
// log and the rows use one word for one state.
QString linkChipToken(reducer::UiLinkState state) {
    return tokens::chipToken(reducer::statusChipKey(state));
}

QString touchpadModeToken(std::uint8_t mode) {
    const std::string_view name = proto::touchpadModeName(mode);
    return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

QVariantList indexList(const std::vector<int>& indices) {
    QVariantList out;
    for (const int index : indices) { out.append(index); }
    return out;
}

QVariantList advertisedList(std::uint16_t caps) {
    QVariantList out;
    for (const auto cap : reducer::advertisedCaps(caps)) { out.append(wireCapToken(cap)); }
    return out;
}

QVariantList buttonList(std::uint16_t wButtons) {
    QVariantList out;
    for (const auto button : input::pressedButtons(wButtons)) {
        out.append(wireButtonToken(button));
    }
    return out;
}

void putReport(QVariantMap& row, const input::GamepadInputProcessor::DeviceState& report) {
    row[QStringLiteral("buttons")] = buttonList(report.wButtons);
    row[QStringLiteral("lx")] = input::stickFraction(report.lx);
    row[QStringLiteral("ly")] = input::stickFraction(report.ly);
    row[QStringLiteral("rx")] = input::stickFraction(report.rx);
    row[QStringLiteral("ry")] = input::stickFraction(report.ry);
    row[QStringLiteral("lt")] = input::triggerFraction(report.lt);
    row[QStringLiteral("rt")] = input::triggerFraction(report.rt);
}

void putMotion(QVariantMap& row, const input::GamepadInputProcessor::MotionSample& motion) {
    row[QStringLiteral("gyroX")] = input::gyroDegreesPerSecond(motion.gyroX);
    row[QStringLiteral("gyroY")] = input::gyroDegreesPerSecond(motion.gyroY);
    row[QStringLiteral("gyroZ")] = input::gyroDegreesPerSecond(motion.gyroZ);
    row[QStringLiteral("accelX")] = input::accelG(motion.accelX);
    row[QStringLiteral("accelY")] = input::accelG(motion.accelY);
    row[QStringLiteral("accelZ")] = input::accelG(motion.accelZ);
}

void putTouch(QVariantMap& row, const input::GamepadInputProcessor::TouchpadSample& touch) {
    row[QStringLiteral("finger0")] = touch.finger0Active;
    row[QStringLiteral("finger0X")] = input::touchFraction(touch.finger0X);
    row[QStringLiteral("finger0Y")] = input::touchFraction(touch.finger0Y);
    row[QStringLiteral("finger1")] = touch.finger1Active;
    row[QStringLiteral("finger1X")] = input::touchFraction(touch.finger1X);
    row[QStringLiteral("finger1Y")] = input::touchFraction(touch.finger1Y);
    row[QStringLiteral("touchClick")] = touch.buttonPressed;
}

void putStickResult(QVariantMap& row, const input::StickTestResult& result) {
    row[QStringLiteral("samples")] = result.samples;
    row[QStringLiteral("driftLeft")] = result.driftLeft;
    row[QStringLiteral("driftRight")] = result.driftRight;
    row[QStringLiteral("suggestedDeadzone")] = result.suggestedDeadzone;
    row[QStringLiteral("reachLeft")] = result.reachLeft;
    row[QStringLiteral("reachRight")] = result.reachRight;
    row[QStringLiteral("circularityLeftKnown")] = result.circularityLeft.has_value();
    row[QStringLiteral("circularityLeft")] = result.circularityLeft.value_or(0.0F);
    row[QStringLiteral("circularityRightKnown")] = result.circularityRight.has_value();
    row[QStringLiteral("circularityRight")] = result.circularityRight.value_or(0.0F);
}

QVariantMap eventRow(const reducer::DiagnosticsEvent& event) {
    QVariantMap row;
    row[QStringLiteral("atMs")] = static_cast<qint64>(event.atMs);
    row[QStringLiteral("kind")] = eventKindToken(event.kind);
    row[QStringLiteral("subject")] = QString::fromStdString(event.subject);
    row[QStringLiteral("from")] = linkChipToken(event.from);
    row[QStringLiteral("to")] = linkChipToken(event.to);
    row[QStringLiteral("path")] = padPathToken(event.path);
    row[QStringLiteral("failure")] = tokens::directFailureToken(event.failure);
    row[QStringLiteral("host")] = QString::fromStdString(event.host);
    return row;
}

} // namespace

HostSessionFacts hostSessionFactsOf(const net::WifiConnection& conn) {
    HostSessionFacts facts;
    facts.id = conn.id();
    facts.offeredProtocol = conn.offeredProtocolVersion();
    facts.settledProtocol = conn.settledProtocolVersion();
    facts.compat = conn.protocolCompat();
    facts.appliedEpoch = conn.lastAppliedEpoch();
    facts.confirmedBitmap = conn.registeredBitmap();
    facts.mouseControlGranted = conn.mouseControlGranted();
    facts.audio = {conn.hostMicAvailable(), conn.hostSpeakerAvailable(),
                   conn.hostHapticAudioAvailable()};
    const auto client = conn.client();
    if (!client) { return facts; }
    facts.live = true;
    facts.hostEpoch = client->serverEpoch();
    facts.hostBitmap = client->serverBitmap();
    facts.backendAvailable = client->backendAvailable();
    facts.activeControllers = client->activeControllerCount();
    facts.missedAcks = client->missedAcks();
    facts.latency = client->latencySummary();
    return facts;
}

QVariantMap hostDiagnosticsRow(const HostSessionFacts& facts) {
    QVariantMap row;
    row[QStringLiteral("id")] = facts.id;
    row[QStringLiteral("live")] = facts.live;
    row[QStringLiteral("offeredProtocol")] = facts.offeredProtocol;
    row[QStringLiteral("settledProtocol")] = facts.settledProtocol;
    row[QStringLiteral("compat")] = tokens::compatToken(facts.compat);
    row[QStringLiteral("appliedEpoch")] = facts.appliedEpoch;
    row[QStringLiteral("hostEpoch")] = facts.hostEpoch;
    row[QStringLiteral("epoch")] =
        agreementToken(reducer::epochAgreement(facts.hostEpoch, facts.appliedEpoch));
    row[QStringLiteral("confirmedControllers")] =
        indexList(reducer::bitmapIndices(facts.confirmedBitmap));
    row[QStringLiteral("hostControllers")] =
        indexList(reducer::hostControllerIndices(facts.hostBitmap));
    row[QStringLiteral("controllers")] =
        agreementToken(reducer::bitmapAgreement(facts.hostBitmap, facts.confirmedBitmap));
    row[QStringLiteral("backend")] =
        hostBackendToken(reducer::hostBackendOf(facts.backendAvailable));
    row[QStringLiteral("activeControllers")] = static_cast<int>(facts.activeControllers);
    row[QStringLiteral("missedAcks")] = facts.missedAcks;
    row[QStringLiteral("rttSamples")] = facts.latency.samples;
    row[QStringLiteral("rttCapacity")] = reducer::kLatencyWindowCapacity;
    row[QStringLiteral("rttP50Ms")] = facts.latency.rttP50Ms;
    row[QStringLiteral("rttP99Ms")] = facts.latency.rttP99Ms;
    row[QStringLiteral("oneWayMs")] = facts.latency.oneWayMs();
    row[QStringLiteral("mouseControl")] = facts.mouseControlGranted;
    row[QStringLiteral("hostMic")] = facts.audio.mic;
    row[QStringLiteral("hostSpeaker")] = facts.audio.speaker;
    row[QStringLiteral("hostHaptics")] = facts.audio.hapticAudio;
    return row;
}

QVariantMap hostCardRow(const composer::ConnectionRow& link, const HostSessionFacts& facts) {
    QVariantMap row = hostDiagnosticsRow(facts);
    row[QStringLiteral("id")] = QString::fromStdString(link.id);
    row[QStringLiteral("label")] = QString::fromStdString(link.label);
    row[QStringLiteral("ip")] = QString::fromStdString(link.ip);
    row[QStringLiteral("udpPort")] = link.udpPort;
    row[QStringLiteral("chip")] = tokens::chipToken(link.chip);
    row[QStringLiteral("dotColor")] = tokens::dotToken(link.dotColor);
    return row;
}

BindingWireFacts bindingWireFactsOf(const net::WifiConnection& conn, const QString& slotId) {
    BindingWireFacts facts;
    const auto descriptor = conn.descriptorFor(slotId);
    if (!descriptor.has_value()) { return facts; }
    facts.declared = true;
    facts.controllerIndex = descriptor->ctrlIdx;
    facts.type = descriptor->type;
    facts.caps = descriptor->caps;
    facts.touchpadMode = descriptor->touchpadMode;
    facts.confirmed = reducer::bitmapHasIndex(conn.registeredBitmap(), descriptor->ctrlIdx);
    const auto client = conn.client();
    const int hostBitmap = client ? client->serverBitmap() : kNotAcked;
    facts.streaming = reducer::streamingOf(hostBitmap, descriptor->ctrlIdx);
    return facts;
}

QVariantMap bindingWireRow(const BindingWireFacts& facts) {
    QVariantMap row;
    row[QStringLiteral("declared")] = facts.declared;
    row[QStringLiteral("controllerIndex")] = facts.controllerIndex;
    row[QStringLiteral("type")] = facts.type;
    row[QStringLiteral("advertised")] = advertisedList(facts.caps);
    row[QStringLiteral("touchpadMode")] = touchpadModeToken(facts.touchpadMode);
    row[QStringLiteral("confirmed")] = facts.confirmed;
    row[QStringLiteral("streaming")] = streamingToken(facts.streaming);
    return row;
}

int bindingTypeOf(const BindingWireFacts& wire, int typeForNextAttach) {
    return wire.declared ? wire.type : typeForNextAttach;
}

QString catalogTypeName(const QList<composer::PickableType>& types, int type) {
    for (const auto& known : types) {
        if (known.type == type) { return known.shortName; }
    }
    return {};
}

int touchpadPickIndex(const QString& choice) {
    if (choice == touchpadChoiceForMode(proto::kTouchpadModeDs4)) { return kTouchpadPickPad; }
    if (choice == touchpadChoiceForMode(proto::kTouchpadModeMouse)) { return kTouchpadPickMouse; }
    return kTouchpadPickOff;
}

QVariantMap inputSnapshotRow(const input::GamepadInputProcessor::Inspection& seen) {
    QVariantMap row;
    row[QStringLiteral("hasReport")] = seen.wire.has_value();
    if (seen.wire.has_value()) { putReport(row, *seen.wire); }
    row[QStringLiteral("hasMotion")] = seen.motion.has_value();
    if (seen.motion.has_value()) { putMotion(row, *seen.motion); }
    row[QStringLiteral("hasTouch")] = seen.touchpad.has_value();
    if (seen.touchpad.has_value()) { putTouch(row, *seen.touchpad); }
    return row;
}

QVariantMap stickTestRow(const input::StickBench& bench, std::int64_t nowMs) {
    QVariantMap row;
    row[QStringLiteral("phase")] = stickTestPhaseToken(bench.phase());
    row[QStringLiteral("kind")] = stickTestKindToken(bench.kind());
    row[QStringLiteral("secondsLeft")] = bench.secondsLeftAt(nowMs);
    if (bench.result().has_value()) { putStickResult(row, *bench.result()); }
    return row;
}

std::optional<input::StickTestKind> stickTestKindFrom(const QString& token) {
    for (const auto kind : kStickTestKinds) {
        if (stickTestKindToken(kind) == token) { return kind; }
    }
    return std::nullopt;
}

std::optional<reducer::BuzzMotor> buzzMotorFrom(const QString& token) {
    for (const auto& word : kBuzzMotorWords) {
        if (token == QLatin1String(word.token)) { return word.motor; }
    }
    return std::nullopt;
}

QVariantList diagnosticsLogRows(const std::vector<reducer::DiagnosticsEvent>& events) {
    QVariantList out;
    for (const auto& event : events) { out.append(eventRow(event)); }
    return out;
}

} // namespace dish::qml
