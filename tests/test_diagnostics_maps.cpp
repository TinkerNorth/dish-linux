// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Diagnostics page's rows. AppViewModel only gathers the inputs and hands
// them to these, so what is pinned here is every token and number the page
// reads, and the two reads that take them off a satellite connection.

#include "Input/GamepadInputProcessor.h"
#include "Models/Models.h"
#include "Network/SatelliteClient.h"
#include "Network/WifiConnection.h"
#include "ServerPacket.h"
#include "core/input/StickHealth.h"
#include "core/model/Protocol.h"
#include "core/reducer/DiagnosticsLog.h"
#include "core/reducer/LatencyWindow.h"
#include "core/reducer/ProtocolNegotiation.h"
#include "qml/DiagnosticsMaps.h"
#include "satellite_client_test_access.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using Catch::Approx;
using dish::composer::ConnectionRow;
using dish::input::GamepadInputProcessor;
using dish::input::StickBench;
using dish::input::StickTestKind;
using dish::models::ControllerApplyDto;
using dish::models::DiscoveredServer;
using dish::net::SatelliteClient;
using dish::net::SatelliteClientTestAccess;
using dish::net::WifiConnection;
using dish::qml::bindingTypeOf;
using dish::qml::BindingWireFacts;
using dish::qml::bindingWireFactsOf;
using dish::qml::bindingWireRow;
using dish::qml::buzzMotorFrom;
using dish::qml::diagnosticsLogRows;
using dish::qml::hostCardRow;
using dish::qml::hostDiagnosticsRow;
using dish::qml::HostSessionFacts;
using dish::qml::hostSessionFactsOf;
using dish::qml::inputSnapshotRow;
using dish::qml::stickTestKindFrom;
using dish::qml::stickTestRow;
using dish::qml::touchpadPickIndex;
using dish::reducer::DiagnosticsEvent;
using dish::reducer::DiagnosticsEventKind;
using dish::reducer::DirectClaimFailure;
using dish::reducer::PadPath;
using dish::reducer::ProtocolCompat;
using dish::reducer::Streaming;
using dish::reducer::UiLinkState;
namespace proto = dish::proto;

namespace {

const QString kSlot = QStringLiteral("sdl:1");

DiscoveredServer server() {
    DiscoveredServer s;
    s.machineId = QStringLiteral("m1");
    s.ip = QStringLiteral("10.0.0.1");
    s.name = QStringLiteral("Desk PC");
    return s;
}

WifiConnection connection() { return WifiConnection(server().id(), server()); }

void attachDualSense(WifiConnection& conn) {
    conn.attachSlot(kSlot, proto::kControllerTypePlayStation, /*hasLightbar=*/true,
                    /*hasMotion=*/true, /*hasRumble=*/true, proto::kTouchpadModeDs4);
}

ControllerApplyDto applied(int ctrlIdx) {
    ControllerApplyDto a;
    a.ctrlIdx = ctrlIdx;
    a.resultCode = proto::kApplyOk;
    return a;
}

// A session the host has acked: on the applied epoch, running both controllers
// this client confirmed, with a seeded latency window.
HostSessionFacts ackedFacts() {
    HostSessionFacts f;
    f.id = QStringLiteral("mid:m1");
    f.live = true;
    f.offeredProtocol = proto::kProtocolVersion;
    f.settledProtocol = proto::kProtocolVersion;
    f.compat = ProtocolCompat::Current;
    f.appliedEpoch = 4;
    f.hostEpoch = 4;
    f.hostBitmap = 0x0003;
    f.confirmedBitmap = 0x0003;
    f.backendAvailable = 1;
    f.activeControllers = 2;
    f.missedAcks = 1;
    f.mouseControlGranted = true;
    f.audio = {true, false, true};
    f.latency = {12, 6.8, 9.5};
    return f;
}

constexpr std::array<std::uint8_t, 4> kToken{0x00, 0x07, 0xA1, 0xB2};
constexpr std::array<std::uint8_t, 32> kKey{0xAB};
// The ping's age when its ack lands, so the round trip is at least this.
constexpr std::int64_t kPingAgeUs = 12'000;
constexpr double kPingAgeMs = 12.0;

std::int64_t steadyNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A session the satellite granted on epoch 4, whose first enriched ack says:
// backend up, one controller, epoch 4, controller 0 active.
void goLive(WifiConnection& conn) {
    auto client = std::make_shared<SatelliteClient>();
    client->setConnectionParams(kToken, kKey, proto::kProtocolVersion);
    conn.markConnecting();
    conn.markConnected(client, QStringLiteral("conn-1"), /*epoch=*/4,
                       /*mouseControlGranted=*/true, {}, {}, {}, {});
    SatelliteClientTestAccess::armPing(*client, steadyNowUs() - kPingAgeUs);
    const auto ack = dish::test::sealServerPacket(kToken, kKey, proto::kMsgHeartbeatAck,
                                                  {0x01, 0x01, 0x00, 0x04, 0x00, 0x01}, 1);
    REQUIRE_FALSE(ack.empty());
    SatelliteClientTestAccess::processIncoming(*client, ack.data(), ack.size());
}

QStringList strings(const QVariant& list) { return list.toStringList(); }

QList<int> ints(const QVariant& list) {
    QList<int> out;
    for (const auto& v : list.toList()) { out.append(v.toInt()); }
    return out;
}

} // namespace

TEST_CASE("an idle connection reads as no session, nothing acked, nothing confirmed",
          "[diagnostics][maps]") {
    const WifiConnection conn = connection();
    const HostSessionFacts f = hostSessionFactsOf(conn);
    CHECK(f.id == QStringLiteral("mid:m1"));
    CHECK_FALSE(f.live);
    CHECK(f.offeredProtocol == proto::kProtocolVersion);
    CHECK(f.settledProtocol == proto::kProtocolVersionMin);
    CHECK(f.compat == ProtocolCompat::Unknown);
    CHECK(f.appliedEpoch == -1);
    CHECK(f.hostEpoch == -1);
    CHECK(f.hostBitmap == -1);
    CHECK(f.confirmedBitmap == 0);
    CHECK(f.backendAvailable == -1);
    CHECK(f.activeControllers == -1);
    CHECK(f.latency.samples == 0);
    CHECK_FALSE(f.mouseControlGranted);
}

TEST_CASE("a connection's negotiation, confirmations and host audio read into the facts",
          "[diagnostics][maps]") {
    WifiConnection conn = connection();
    conn.setSettledProtocolVersion(2, /*satelliteBehind=*/true);
    conn.setProtocolCompat(ProtocolCompat::SatelliteUpdateAvailable);
    conn.setHostControllerAudio(/*mic=*/true, /*speaker=*/false, /*hapticAudio=*/true);
    attachDualSense(conn);
    conn.applyResults({applied(0)});

    const HostSessionFacts f = hostSessionFactsOf(conn);
    CHECK(f.settledProtocol == 2);
    CHECK(f.compat == ProtocolCompat::SatelliteUpdateAvailable);
    CHECK(f.confirmedBitmap == 0x0001);
    CHECK(f.audio.mic);
    CHECK_FALSE(f.audio.speaker);
    CHECK(f.audio.hapticAudio);
}

TEST_CASE("a live connection reads its session off the client and its first ack",
          "[diagnostics][maps]") {
    WifiConnection conn = connection();
    goLive(conn);
    const HostSessionFacts f = hostSessionFactsOf(conn);
    CHECK(f.live);
    CHECK(f.appliedEpoch == 4);
    CHECK(f.mouseControlGranted);
    CHECK(f.hostEpoch == 4);
    CHECK(f.hostBitmap == 0x0001);
    CHECK(f.backendAvailable == 1);
    CHECK(f.activeControllers == 1);
    CHECK(f.latency.samples == 1);
    CHECK(f.latency.rttP50Ms >= kPingAgeMs);
    conn.markDisconnected();
}

TEST_CASE("a host row before the first enriched ack reads every ack answer as unknown",
          "[diagnostics][maps]") {
    HostSessionFacts f;
    f.confirmedBitmap = 0x0001;
    const QVariantMap row = hostDiagnosticsRow(f);
    CHECK(row.value("epoch").toString() == QStringLiteral("unknown"));
    CHECK(row.value("controllers").toString() == QStringLiteral("unknown"));
    CHECK(row.value("backend").toString() == QStringLiteral("unknown"));
    CHECK(row.value("hostControllers").toList().isEmpty());
    CHECK(ints(row.value("confirmedControllers")) == QList<int>{0});
    CHECK(row.value("compat").toString() == QStringLiteral("unknown"));
    CHECK_FALSE(row.value("live").toBool());
}

TEST_CASE("an acked host row reads in step, with its round trips and its audio",
          "[diagnostics][maps]") {
    const QVariantMap row = hostDiagnosticsRow(ackedFacts());
    CHECK(row.value("id").toString() == QStringLiteral("mid:m1"));
    CHECK(row.value("live").toBool());
    CHECK(row.value("settledProtocol").toInt() == proto::kProtocolVersion);
    CHECK(row.value("offeredProtocol").toInt() == proto::kProtocolVersion);
    CHECK(row.value("compat").toString() == QStringLiteral("current"));
    CHECK(row.value("appliedEpoch").toInt() == 4);
    CHECK(row.value("hostEpoch").toInt() == 4);
    CHECK(row.value("epoch").toString() == QStringLiteral("inStep"));
    CHECK(ints(row.value("hostControllers")) == QList<int>{0, 1});
    CHECK(ints(row.value("confirmedControllers")) == QList<int>{0, 1});
    CHECK(row.value("controllers").toString() == QStringLiteral("inStep"));
    CHECK(row.value("backend").toString() == QStringLiteral("available"));
    CHECK(row.value("activeControllers").toInt() == 2);
    CHECK(row.value("missedAcks").toInt() == 1);
    CHECK(row.value("rttSamples").toInt() == 12);
    CHECK(row.value("rttCapacity").toInt() == dish::reducer::kLatencyWindowCapacity);
    CHECK(row.value("rttP50Ms").toDouble() == 6.8);
    CHECK(row.value("rttP99Ms").toDouble() == 9.5);
    CHECK(row.value("oneWayMs").toDouble() == 3.4);
    CHECK(row.value("mouseControl").toBool());
    CHECK(row.value("hostMic").toBool());
    CHECK_FALSE(row.value("hostSpeaker").toBool());
    CHECK(row.value("hostHaptics").toBool());
}

TEST_CASE("a host that moved past the applied epoch and bitmap reads diverged",
          "[diagnostics][maps]") {
    HostSessionFacts f = ackedFacts();
    f.hostEpoch = 5;
    f.hostBitmap = 0x0001;
    f.backendAvailable = 0;
    const QVariantMap row = hostDiagnosticsRow(f);
    CHECK(row.value("epoch").toString() == QStringLiteral("diverged"));
    CHECK(row.value("controllers").toString() == QStringLiteral("diverged"));
    CHECK(row.value("backend").toString() == QStringLiteral("unavailable"));
}

TEST_CASE("a host card names the host by its link, beside the session's own row",
          "[diagnostics][maps]") {
    ConnectionRow link;
    link.id = "mid:m1";
    link.label = "Desk PC";
    link.ip = "10.0.0.1";
    link.udpPort = 9876;
    link.chip = dish::reducer::StatusChipKey::Online;
    link.dotColor = dish::reducer::DotColor::Success;
    HostSessionFacts facts = ackedFacts();
    facts.id.clear();
    const QVariantMap row = hostCardRow(link, facts);
    CHECK(row.value("id").toString() == QStringLiteral("mid:m1"));
    CHECK(row.value("label").toString() == QStringLiteral("Desk PC"));
    CHECK(row.value("ip").toString() == QStringLiteral("10.0.0.1"));
    CHECK(row.value("udpPort").toInt() == 9876);
    CHECK(row.value("chip").toString() == QStringLiteral("online"));
    CHECK(row.value("dotColor").toString() == QStringLiteral("success"));
    CHECK(row.value("epoch").toString() == QStringLiteral("inStep"));
}

TEST_CASE("a slot the connection carries no descriptor for declares nothing",
          "[diagnostics][maps]") {
    const WifiConnection conn = connection();
    REQUIRE_FALSE(bindingWireFactsOf(conn, kSlot).declared);
}

TEST_CASE("an attached slot reads its descriptor, unconfirmed until the host applies it",
          "[diagnostics][maps]") {
    WifiConnection conn = connection();
    // A pad already on the connection holds index 0, so this one declares index 1
    // with its touchpad off: neither matches a BindingWireFacts default.
    conn.attachSlot(QStringLiteral("sdl:0"), proto::kControllerTypeXbox, /*hasLightbar=*/false,
                    /*hasMotion=*/false, /*hasRumble=*/true, proto::kTouchpadModeOff);
    conn.attachSlot(kSlot, proto::kControllerTypePlayStation, /*hasLightbar=*/true,
                    /*hasMotion=*/true, /*hasRumble=*/true, proto::kTouchpadModeOff);
    const BindingWireFacts f = bindingWireFactsOf(conn, kSlot);
    CHECK(f.declared);
    CHECK(f.controllerIndex == 1);
    CHECK(f.type == proto::kControllerTypePlayStation);
    CHECK((f.caps & proto::kCapMotion) != 0);
    CHECK(f.touchpadMode == proto::kTouchpadModeOff);
    CHECK_FALSE(f.confirmed);
    CHECK(f.streaming == Streaming::Unknown);
}

TEST_CASE("a slot the host applied reads as confirmed", "[diagnostics][maps]") {
    WifiConnection conn = connection();
    attachDualSense(conn);
    conn.applyResults({applied(0)});
    REQUIRE(bindingWireFactsOf(conn, kSlot).confirmed);
}

TEST_CASE("a slot on a live connection streams when the host's ack names its controller",
          "[diagnostics][maps]") {
    WifiConnection conn = connection();
    attachDualSense(conn);
    goLive(conn);
    CHECK(bindingWireFactsOf(conn, kSlot).streaming == Streaming::Yes);
    conn.markDisconnected();
}

TEST_CASE("a binding row names its advertised caps with the capability table's words",
          "[diagnostics][maps]") {
    BindingWireFacts f;
    f.declared = true;
    f.controllerIndex = 2;
    f.caps = proto::kCapAnalogTriggers | proto::kCapRumble | proto::kCapMotion |
             proto::kCapLightbar | proto::kCapTriggerEffects | proto::kCapPlayerLeds |
             proto::kCapMic | proto::kCapSpeaker | proto::kCapHapticAudio;
    f.touchpadMode = proto::kTouchpadModeMouse;
    f.confirmed = true;
    f.streaming = Streaming::Yes;
    const QVariantMap row = bindingWireRow(f);
    CHECK(row.value("declared").toBool());
    CHECK(row.value("controllerIndex").toInt() == 2);
    CHECK(strings(row.value("advertised")) ==
          QStringList{"triggers", "rumble", "motion", "lightbar", "triggerEffects", "playerLeds",
                      "mic", "speaker", "hapticAudio"});
    CHECK(row.value("touchpadMode").toString() == QStringLiteral("mouse"));
    CHECK(row.value("confirmed").toBool());
    CHECK(row.value("streaming").toString() == QStringLiteral("yes"));
}

TEST_CASE("the streaming answer reads as a token", "[diagnostics][maps]") {
    BindingWireFacts f;
    f.streaming = Streaming::No;
    CHECK(bindingWireRow(f).value("streaming").toString() == QStringLiteral("no"));
    f.streaming = Streaming::Unknown;
    CHECK(bindingWireRow(f).value("streaming").toString() == QStringLiteral("unknown"));
}

TEST_CASE("a declared binding is solved for the type on the wire", "[diagnostics][maps]") {
    BindingWireFacts wire;
    wire.declared = true;
    wire.type = proto::kControllerTypePlayStation;
    CHECK(bindingTypeOf(wire, proto::kControllerTypeXbox) == proto::kControllerTypePlayStation);
}

TEST_CASE("an undeclared binding is solved for the type its next attach would carry",
          "[diagnostics][maps]") {
    BindingWireFacts wire;
    wire.type = proto::kControllerTypePlayStation;
    CHECK(bindingTypeOf(wire, proto::kControllerTypeXbox) == proto::kControllerTypeXbox);
}

TEST_CASE("the touchpad choice reads as the capability rows' mode", "[diagnostics][maps]") {
    CHECK(touchpadPickIndex(QStringLiteral("pad")) == 1);
    CHECK(touchpadPickIndex(QStringLiteral("mouse")) == 2);
    CHECK(touchpadPickIndex(QStringLiteral("off")) == 0);
    CHECK(touchpadPickIndex(QString()) == 0);
}

TEST_CASE("a wire name is no touchpad choice and reads as off", "[diagnostics][maps]") {
    CHECK(touchpadPickIndex(QStringLiteral("ds4")) == 0);
}

TEST_CASE("a pad that reported nothing reads as no report, no motion and no touch",
          "[diagnostics][maps]") {
    const QVariantMap row = inputSnapshotRow({});
    CHECK_FALSE(row.value("hasReport").toBool());
    CHECK_FALSE(row.value("hasMotion").toBool());
    CHECK_FALSE(row.value("hasTouch").toBool());
    CHECK_FALSE(row.contains("buttons"));
}

TEST_CASE("a report reads its buttons as tokens and its axes as fractions", "[diagnostics][maps]") {
    GamepadInputProcessor::Inspection seen;
    GamepadInputProcessor::DeviceState report;
    report.wButtons = GamepadInputProcessor::Buttons::kB | GamepadInputProcessor::Buttons::kA;
    report.lx = 16384;
    report.ry = -32768;
    report.rt = 255;
    seen.wire = report;
    const QVariantMap row = inputSnapshotRow(seen);
    CHECK(row.value("hasReport").toBool());
    CHECK(strings(row.value("buttons")) == QStringList{"a", "b"});
    CHECK(row.value("lx").toDouble() == 0.5);
    CHECK(row.value("ry").toDouble() == -1.0);
    CHECK(row.value("lt").toDouble() == 0.0);
    CHECK(row.value("rt").toDouble() == 1.0);
}

TEST_CASE("a motion sample reads in degrees per second and in g", "[diagnostics][maps]") {
    GamepadInputProcessor::Inspection seen;
    GamepadInputProcessor::MotionSample motion;
    motion.gyroZ = 32767;
    motion.accelY = -32767;
    seen.motion = motion;
    const QVariantMap row = inputSnapshotRow(seen);
    CHECK(row.value("hasMotion").toBool());
    CHECK(row.value("gyroZ").toDouble() == Approx(2000.0));
    CHECK(row.value("accelY").toDouble() == Approx(-4.0));
    CHECK(row.value("gyroX").toDouble() == 0.0);
}

TEST_CASE("a touch frame reads its fingers as fractions across the pad", "[diagnostics][maps]") {
    GamepadInputProcessor::Inspection seen;
    GamepadInputProcessor::TouchpadSample touch;
    touch.finger0Active = true;
    touch.finger0X = 32767;
    touch.finger0Y = -32768;
    touch.buttonPressed = true;
    seen.touchpad = touch;
    const QVariantMap row = inputSnapshotRow(seen);
    CHECK(row.value("hasTouch").toBool());
    CHECK(row.value("finger0").toBool());
    CHECK(row.value("finger0X").toDouble() == 1.0);
    CHECK(row.value("finger0Y").toDouble() == 0.0);
    CHECK_FALSE(row.value("finger1").toBool());
    CHECK(row.value("touchClick").toBool());
}

TEST_CASE("an idle bench reads idle with no result", "[diagnostics][maps]") {
    const QVariantMap row = stickTestRow(StickBench{}, 0);
    CHECK(row.value("phase").toString() == QStringLiteral("idle"));
    CHECK_FALSE(row.contains("samples"));
}

TEST_CASE("a running test reads its kind and its seconds left", "[diagnostics][maps]") {
    StickBench bench;
    bench.start(StickTestKind::Range, 0);
    const QVariantMap row = stickTestRow(bench, 1'000);
    CHECK(row.value("phase").toString() == QStringLiteral("running"));
    CHECK(row.value("kind").toString() == QStringLiteral("range"));
    CHECK(row.value("secondsLeft").toInt() == 8);
}

TEST_CASE("a finished drift test reads its figures, and a roundness it never measured",
          "[diagnostics][maps]") {
    StickBench bench;
    bench.start(StickTestKind::Drift, 0);
    bench.sample({0.06F, 0.08F}, {0.0F, 0.2F});
    bench.advance(dish::input::kDriftCaptureMs);
    const QVariantMap row = stickTestRow(bench, dish::input::kDriftCaptureMs);
    CHECK(row.value("phase").toString() == QStringLiteral("done"));
    CHECK(row.value("kind").toString() == QStringLiteral("drift"));
    CHECK(row.value("samples").toInt() == 1);
    CHECK(row.value("driftLeft").toDouble() == Approx(0.1).epsilon(0.001));
    CHECK(row.value("driftRight").toDouble() == Approx(0.2).epsilon(0.001));
    CHECK(row.value("suggestedDeadzone").toDouble() == Approx(0.3).epsilon(0.001));
    CHECK_FALSE(row.value("circularityLeftKnown").toBool());
    CHECK_FALSE(row.value("circularityRightKnown").toBool());
}

TEST_CASE("a stick test's kind reads back from the word its row writes", "[diagnostics][maps]") {
    StickBench range;
    range.start(StickTestKind::Range, 0);
    CHECK(stickTestKindFrom(stickTestRow(range, 0).value("kind").toString()) ==
          StickTestKind::Range);
    CHECK(stickTestKindFrom(QStringLiteral("drift")) == StickTestKind::Drift);
    CHECK_FALSE(stickTestKindFrom(QStringLiteral("sweep")).has_value());
}

TEST_CASE("the bench's motor words read as the motors they run", "[diagnostics][maps]") {
    CHECK(buzzMotorFrom(QStringLiteral("weak")) == dish::reducer::BuzzMotor::Weak);
    CHECK(buzzMotorFrom(QStringLiteral("strong")) == dish::reducer::BuzzMotor::Strong);
    CHECK(buzzMotorFrom(QStringLiteral("both")) == dish::reducer::BuzzMotor::Both);
    CHECK_FALSE(buzzMotorFrom(QStringLiteral("left")).has_value());
}

TEST_CASE("a log row reads a link change as the chips it moved between", "[diagnostics][maps]") {
    DiagnosticsEvent e;
    e.atMs = 1'700'000'000'000;
    e.kind = DiagnosticsEventKind::LinkChanged;
    e.subject = "Desk PC";
    e.from = UiLinkState::Connected;
    e.to = UiLinkState::Unstable;
    const QVariantList rows = diagnosticsLogRows({e});
    REQUIRE(rows.size() == 1);
    const QVariantMap row = rows.front().toMap();
    CHECK(row.value("atMs").toLongLong() == 1'700'000'000'000);
    CHECK(row.value("kind").toString() == QStringLiteral("linkChanged"));
    CHECK(row.value("subject").toString() == QStringLiteral("Desk PC"));
    CHECK(row.value("from").toString() == QStringLiteral("online"));
    CHECK(row.value("to").toString() == QStringLiteral("unstable"));
}

TEST_CASE("a log row reads a pad's path, failure and host as tokens", "[diagnostics][maps]") {
    DiagnosticsEvent failed;
    failed.kind = DiagnosticsEventKind::PadDirectFailed;
    failed.subject = "DualSense";
    failed.path = PadPath::UsbDirect;
    failed.failure = DirectClaimFailure::PermissionDenied;
    failed.host = "Desk PC";
    const QVariantMap row = diagnosticsLogRows({failed}).front().toMap();
    CHECK(row.value("kind").toString() == QStringLiteral("padDirectFailed"));
    CHECK(row.value("path").toString() == QStringLiteral("usbDirect"));
    CHECK(row.value("failure").toString() == QStringLiteral("permissionDenied"));
    CHECK(row.value("host").toString() == QStringLiteral("Desk PC"));
}

TEST_CASE("the log rows keep the recorder's order, oldest first", "[diagnostics][maps]") {
    DiagnosticsEvent first;
    first.atMs = 1;
    first.kind = DiagnosticsEventKind::PadAttached;
    DiagnosticsEvent second;
    second.atMs = 2;
    second.kind = DiagnosticsEventKind::PadBound;
    const QVariantList rows = diagnosticsLogRows({first, second});
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].toMap().value("kind").toString() == QStringLiteral("padAttached"));
    CHECK(rows[1].toMap().value("kind").toString() == QStringLiteral("padBound"));
}
