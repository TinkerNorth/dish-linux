// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The diagnostics bundle, `dish-diagnostics.v1` (docs/diagnostics-bundle.md):
// its shape, and what it never holds. The fixture is a machine with a paired
// host whose name, address, pairing key, certificate fingerprint and Wi-Fi
// network are all known to the rest of the app; the flight recorder names the
// host by its label in every line about it. None of those values may appear
// anywhere in the file.

#include "composer/DiagnosticsBundle.h"
#include "core/diagnostics/Json.h"
#include "core/diagnostics/PadLanes.h"
#include "core/diagnostics/UsbDescriptors.h"
#include "core/diagnostics/WireCounters.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cerrno>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using dish::composer::buildDiagnosticsBundle;
using dish::composer::BundleController;
using dish::composer::BundleFacts;
using dish::composer::BundleHost;
using dish::composer::BundleLogEvent;
using dish::diagnostics::Json;
using dish::diagnostics::OutputLane;
using dish::diagnostics::SnapshotError;

namespace {

constexpr const char* kHostName = "Living Room PC";
constexpr const char* kHostAddress = "192.168.1.42";
constexpr const char* kPairingKey =
    "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
constexpr const char* kFingerprint = "AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89";
constexpr const char* kWifiName = "TinkerNet-5G";
constexpr const char* kSecondHost = "Bedroom Steam Deck";

struct BundleFixture {
    BundleFacts facts;

    BundleFixture() {
        facts.createdAtMs = 1795122251000;
        facts.appVersion = "2.8.0";
        facts.model = "AYN Thor";
        facts.os = "Ubuntu 24.04.1 LTS (6.8.0-45-generic)";
        facts.arch = "x86_64";
        facts.controllerDb = {1, 1, "bundled", 1795122000000, "upToDate"};

        BundleController pad;
        pad.record = "sony-dualsense";
        pad.verification = "verified";
        pad.transport = "usb";
        pad.path = "direct";
        pad.hardware.vendorId = 0x054C;
        pad.hardware.productId = 0x0CE6;
        pad.hardware.product = "Wireless Controller";
        pad.hardware.manufacturer = "Sony Interactive Entertainment";
        pad.hardware.endpointsRead = true;
        pad.hardware.endpointIn = 0x84;
        pad.hardware.endpointOut = 0x03;
        pad.expectMismatch = {"endpoints.out: expected true, found false"};
        pad.lastWrite = dish::diagnostics::WriteVerdict{
            true, dish::diagnostics::WriteRoute::InterruptOut, 0x02, false, EPIPE, 1795122250000};
        pad.outcomes = {{"rumble.strong", "worked", 1795122240000}};
        facts.controllers.push_back(pad);

        BundleHost host;
        host.kind = "satellite";
        host.protocol = 4;
        host.link = "connected";
        host.speaker = true;
        facts.hosts.push_back(host);
        facts.hostLabels.push_back(kHostName);
        facts.hostAddresses.push_back(kHostAddress);

        facts.log.push_back(
            BundleLogEvent{1795122100000, "linkAppeared", kHostName, true, "", {"", "connected"}});
        facts.log.push_back(
            BundleLogEvent{1795122110000, "padBound", "Wireless Controller", false, kHostName, {}});
        // A host the list no longer has, named by its address because it never had a name.
        facts.log.push_back(
            BundleLogEvent{1795122120000, "linkRemoved", kHostAddress, true, "", {}});
        facts.log.push_back(BundleLogEvent{
            1795122130000, "padUnbound", "Wireless Controller", false, kSecondHost, {}});
    }

    QJsonObject parsed() const {
        return QJsonDocument::fromJson(QByteArray::fromStdString(buildDiagnosticsBundle(facts)))
            .object();
    }
};

} // namespace

TEST_CASE("diagnostics bundle: never a key, a fingerprint, an address, a Wi-Fi or a host name",
          "[diagnostics][bundle]") {
    const BundleFixture fixture;
    const std::string bundle = buildDiagnosticsBundle(fixture.facts);
    for (const std::string_view forbidden :
         {kHostName, kHostAddress, kPairingKey, kFingerprint, kWifiName, kSecondHost}) {
        INFO(forbidden);
        CHECK(bundle.find(forbidden) == std::string::npos);
    }
}

TEST_CASE("diagnostics bundle: hosts are aliases, in the log as in the host list",
          "[diagnostics][bundle]") {
    const QJsonObject root = BundleFixture().parsed();
    const QJsonObject host = root[QStringLiteral("hosts")].toArray()[0].toObject();
    CHECK(host[QStringLiteral("alias")].toString() == QStringLiteral("Host 1"));
    CHECK(host[QStringLiteral("kind")].toString() == QStringLiteral("satellite"));
    CHECK(host[QStringLiteral("protocol")].toInt() == 4);
    CHECK(host[QStringLiteral("link")].toString() == QStringLiteral("connected"));
    CHECK(host[QStringLiteral("grants")].toObject()[QStringLiteral("speaker")].toBool());
    CHECK(host[QStringLiteral("version")].isNull());
    CHECK(host[QStringLiteral("session")].isNull());
    CHECK(host[QStringLiteral("snapshot")].isNull());
    CHECK(host[QStringLiteral("snapshotError")].isNull());

    const QJsonArray log = root[QStringLiteral("log")].toArray();
    REQUIRE(log.size() == 4);
    CHECK(log[0].toObject()[QStringLiteral("message")].toString() ==
          QStringLiteral("Host 1 connected"));
    CHECK(log[1].toObject()[QStringLiteral("message")].toString() ==
          QStringLiteral("Wireless Controller Host 1"));
    CHECK(log[2].toObject()[QStringLiteral("message")].toString() == QStringLiteral("Host 2"));
    CHECK(log[3].toObject()[QStringLiteral("message")].toString() ==
          QStringLiteral("Wireless Controller Host 3"));
    CHECK(log[1].toObject()[QStringLiteral("tag")].toString() == QStringLiteral("padBound"));
}

TEST_CASE("diagnostics bundle: the envelope", "[diagnostics][bundle]") {
    const QJsonObject root = BundleFixture().parsed();
    CHECK(root[QStringLiteral("bundle")].toInt() == 1);
    CHECK(root[QStringLiteral("createdAt")].toString() == QStringLiteral("2026-11-19T21:04:11Z"));
    const QJsonObject app = root[QStringLiteral("app")].toObject();
    CHECK(app[QStringLiteral("name")].toString() == QStringLiteral("dish-linux"));
    CHECK(app[QStringLiteral("version")].toString() == QStringLiteral("2.8.0"));
    CHECK(app[QStringLiteral("build")].toString() == QStringLiteral("20800"));
    CHECK(app[QStringLiteral("flavor")].toString() == QStringLiteral("github"));
    const QJsonObject device = root[QStringLiteral("device")].toObject();
    CHECK(device[QStringLiteral("model")].toString() == QStringLiteral("AYN Thor"));
    const QJsonObject db = root[QStringLiteral("controllerDb")].toObject();
    CHECK(db[QStringLiteral("bundledVersion")].toInt() == 1);
    CHECK(db[QStringLiteral("source")].toString() == QStringLiteral("bundled"));
    CHECK(db[QStringLiteral("lastCheckResult")].toString() == QStringLiteral("upToDate"));
    CHECK(db[QStringLiteral("lastCheckAt")].toString() == QStringLiteral("2026-11-19T21:00:00Z"));
}

TEST_CASE("diagnostics bundle: a never-checked database writes nulls, not zeroes",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.controllerDb = {1, 1, "bundled", 0, ""};
    const QJsonObject db = fixture.parsed()[QStringLiteral("controllerDb")].toObject();
    CHECK(db[QStringLiteral("lastCheckAt")].isNull());
    CHECK(db[QStringLiteral("lastCheckResult")].isNull());
}

TEST_CASE("diagnostics bundle: a controller with its record, hardware, write and outcomes",
          "[diagnostics][bundle]") {
    const QJsonObject pad =
        BundleFixture().parsed()[QStringLiteral("controllers")].toArray()[0].toObject();
    CHECK(pad[QStringLiteral("record")].toString() == QStringLiteral("sony-dualsense"));
    CHECK(pad[QStringLiteral("verification")].toString() == QStringLiteral("verified"));
    CHECK(pad[QStringLiteral("path")].toString() == QStringLiteral("direct"));
    const QJsonObject hw = pad[QStringLiteral("hardware")].toObject();
    CHECK(hw[QStringLiteral("vid")].toString() == QStringLiteral("054c"));
    CHECK(hw[QStringLiteral("pid")].toString() == QStringLiteral("0ce6"));
    const QJsonObject endpoints = hw[QStringLiteral("endpoints")].toObject();
    CHECK(endpoints[QStringLiteral("in")].toString() == QStringLiteral("84"));
    CHECK(endpoints[QStringLiteral("out")].toString() == QStringLiteral("03"));
    CHECK(pad[QStringLiteral("expectMismatch")].toArray()[0].toString() ==
          QStringLiteral("endpoints.out: expected true, found false"));
    const QJsonObject write = pad[QStringLiteral("lastWrite")].toObject();
    CHECK(write[QStringLiteral("route")].toString() == QStringLiteral("interruptOut"));
    CHECK(write[QStringLiteral("reportId")].toString() == QStringLiteral("02"));
    CHECK_FALSE(write[QStringLiteral("ok")].toBool());
    CHECK(write[QStringLiteral("err")].toString() == QStringLiteral("EPIPE"));
    const QJsonObject outcome = pad[QStringLiteral("outcomes")].toArray()[0].toObject();
    CHECK(outcome[QStringLiteral("test")].toString() == QStringLiteral("rumble.strong"));
    CHECK(outcome[QStringLiteral("result")].toString() == QStringLiteral("worked"));
    CHECK(pad[QStringLiteral("lastFeedback")].isNull());
}

TEST_CASE("diagnostics bundle: a pad with no record and no write", "[diagnostics][bundle]") {
    BundleFixture fixture;
    BundleController bare;
    bare.transport = "bluetooth";
    bare.path = "sdl";
    fixture.facts.controllers = {bare};
    const QJsonObject pad = fixture.parsed()[QStringLiteral("controllers")].toArray()[0].toObject();
    CHECK(pad[QStringLiteral("record")].isNull());
    CHECK(pad[QStringLiteral("lastWrite")].isNull());
    CHECK(pad[QStringLiteral("hardware")].toObject()[QStringLiteral("endpoints")].isNull());
    CHECK(pad[QStringLiteral("outcomes")].toArray().isEmpty());
}

TEST_CASE("diagnostics bundle: an interface with no OUT endpoint says so with a null",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.controllers[0].hardware.endpointOut.reset();
    const QJsonObject endpoints = fixture.parsed()[QStringLiteral("controllers")]
                                      .toArray()[0]
                                      .toObject()[QStringLiteral("hardware")]
                                      .toObject()[QStringLiteral("endpoints")]
                                      .toObject();
    CHECK(endpoints[QStringLiteral("in")].toString() == QStringLiteral("84"));
    CHECK(endpoints[QStringLiteral("out")].isNull());
}

TEST_CASE("diagnostics bundle: the flavor names how this copy was installed",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.flavor = "flatpak";
    const QJsonObject app = fixture.parsed()[QStringLiteral("app")].toObject();
    CHECK(app[QStringLiteral("flavor")].toString() == QStringLiteral("flatpak"));
}

// ── Second cut: the report, both ends of each host, every path ─────────────

namespace {

// A host's own snapshot as Satellite would send it, carrying the two things a careless build
// might leave in: this host's name and its address.
Json carelessSnapshot() {
    Json satellite = Json::object();
    satellite.set("version", "2.3.0");
    satellite.set("note", std::string("served by ") + kHostName + " at " + kHostAddress);
    Json snapshot = Json::object();
    snapshot.set("format", "satellite-connection-diagnostics.v1");
    snapshot.set("satellite", std::move(satellite));
    snapshot.set(kHostAddress, true);
    return snapshot;
}

dish::composer::BundleCapabilities cachedCapabilities() {
    dish::composer::BundleCapabilities caps;
    caps.serverVersion = "2.2.0";
    caps.protocol = 4;
    caps.backends = {{"hidmaestro", true, true, true}, {"vigem", true, false, false}};
    caps.controllerAudio = dish::composer::BundleControllerAudio{true, false, true, false};
    caps.catalogVersion = 2;
    return caps;
}

dish::composer::BundleSession liveSession() {
    dish::composer::BundleSession session;
    dish::composer::BundleSessionSlot slot;
    slot.index = 0;
    slot.record = "sony-dualsense";
    slot.requestedType = "dualsense";
    slot.appliedType = "dualsense";
    slot.backend = "hidmaestro";
    slot.result = "ok";
    slot.touchpadMode = "ds4";
    session.controllerSlots.push_back(slot);
    session.ack = dish::composer::BundleAck{true, 1, 12, 0x0001};
    session.rtt = dish::composer::BundleRtt{2.1, 4.8, 120};
    dish::diagnostics::WireCounters counters;
    for (int i = 0; i < 3; ++i) { counters.countSent(dish::proto::kMsgInput); }
    counters.countReceived(dish::proto::kMsgRumble);
    session.wire = counters.counts();
    return session;
}

} // namespace

TEST_CASE("diagnostics bundle: the person's report goes in first", "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.report =
        dish::composer::BundleReport{"noFeedback", "Returnal never rumbles.", "sony-dualsense"};
    const QJsonObject root = fixture.parsed();
    const QJsonObject report = root[QStringLiteral("report")].toObject();
    CHECK(report[QStringLiteral("category")].toString() == QStringLiteral("noFeedback"));
    CHECK(report[QStringLiteral("text")].toString() == QStringLiteral("Returnal never rumbles."));
    CHECK(report[QStringLiteral("controller")].toString() == QStringLiteral("sony-dualsense"));
    const std::string bundle = buildDiagnosticsBundle(fixture.facts);
    CHECK(bundle.find("\"report\"") < bundle.find("\"app\""));
}

TEST_CASE("diagnostics bundle: a report about no controller says null", "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.report = dish::composer::BundleReport{"question", "How do I pair?", std::nullopt};
    const QJsonObject report = fixture.parsed()[QStringLiteral("report")].toObject();
    CHECK(report[QStringLiteral("controller")].isNull());
    CHECK_FALSE(BundleFixture().parsed().contains(QStringLiteral("report")));
}

TEST_CASE("diagnostics bundle: the session as this client saw it", "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.hosts[0].session = liveSession();
    const QJsonObject session = fixture.parsed()[QStringLiteral("hosts")]
                                    .toArray()[0]
                                    .toObject()[QStringLiteral("session")]
                                    .toObject();
    const QJsonObject slot = session[QStringLiteral("slots")].toArray()[0].toObject();
    CHECK(slot[QStringLiteral("index")].toInt() == 0);
    CHECK(slot[QStringLiteral("record")].toString() == QStringLiteral("sony-dualsense"));
    CHECK(slot[QStringLiteral("requestedType")].toString() == QStringLiteral("dualsense"));
    CHECK(slot[QStringLiteral("appliedType")].toString() == QStringLiteral("dualsense"));
    CHECK(slot[QStringLiteral("backend")].toString() == QStringLiteral("hidmaestro"));
    CHECK(slot[QStringLiteral("result")].toString() == QStringLiteral("ok"));
    CHECK(slot[QStringLiteral("touchpadMode")].toString() == QStringLiteral("ds4"));
    const QJsonObject ack = session[QStringLiteral("ack")].toObject();
    CHECK(ack[QStringLiteral("backendAvailable")].toBool());
    CHECK(ack[QStringLiteral("activeControllers")].toInt() == 1);
    CHECK(ack[QStringLiteral("epoch")].toInt() == 12);
    CHECK(ack[QStringLiteral("activeBitmap")].toString() == QStringLiteral("0001"));
    const QJsonObject rtt = session[QStringLiteral("rtt")].toObject();
    CHECK(rtt[QStringLiteral("p50Ms")].toDouble() == 2.1);
    CHECK(rtt[QStringLiteral("p99Ms")].toDouble() == 4.8);
    CHECK(rtt[QStringLiteral("samples")].toInt() == 120);
    const QJsonObject sent = session[QStringLiteral("sent")].toObject();
    CHECK(sent[QStringLiteral("input")].toInteger() == 3);
    CHECK(sent[QStringLiteral("mic")].toInteger() == 0);
    const QJsonObject received = session[QStringLiteral("received")].toObject();
    CHECK(received[QStringLiteral("rumble")].toInteger() == 1);
    CHECK(received.contains(QStringLiteral("sessionClose")));
}

TEST_CASE("diagnostics bundle: before the first ack and the first round trip, nulls",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    dish::composer::BundleSession session;
    session.controllerSlots.push_back(dish::composer::BundleSessionSlot{});
    fixture.facts.hosts[0].session = session;
    const QJsonObject parsed = fixture.parsed()[QStringLiteral("hosts")]
                                   .toArray()[0]
                                   .toObject()[QStringLiteral("session")]
                                   .toObject();
    CHECK(parsed[QStringLiteral("ack")].isNull());
    CHECK(parsed[QStringLiteral("rtt")].isNull());
    CHECK(parsed[QStringLiteral("sent")].isNull());
    const QJsonObject slot = parsed[QStringLiteral("slots")].toArray()[0].toObject();
    CHECK(slot[QStringLiteral("index")].isNull());
    CHECK(slot[QStringLiteral("appliedType")].isNull());
}

TEST_CASE("diagnostics bundle: a Satellite's own snapshot is embedded, scrubbed of its name",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.hosts[0].hostSnapshot = carelessSnapshot();
    fixture.facts.hosts[0].capabilities = cachedCapabilities();
    const std::string bundle = buildDiagnosticsBundle(fixture.facts);
    CHECK(bundle.find(kHostName) == std::string::npos);
    CHECK(bundle.find(kHostAddress) == std::string::npos);
    const QJsonObject host = fixture.parsed()[QStringLiteral("hosts")].toArray()[0].toObject();
    const QJsonObject snapshot = host[QStringLiteral("snapshot")].toObject();
    CHECK(snapshot[QStringLiteral("format")].toString() ==
          QStringLiteral("satellite-connection-diagnostics.v1"));
    CHECK(snapshot[QStringLiteral("satellite")].toObject()[QStringLiteral("note")].toString() ==
          QStringLiteral("served by Host 1 at Host 1"));
    CHECK(snapshot[QStringLiteral("Host 1")].toBool());
    CHECK(host[QStringLiteral("snapshotError")].isNull());
}

TEST_CASE("diagnostics bundle: an older Satellite is described from what this client fetched",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.hosts[0].capabilities = cachedCapabilities();
    fixture.facts.hosts[0].snapshotError = SnapshotError::OlderSatellite;
    const QJsonObject host = fixture.parsed()[QStringLiteral("hosts")].toArray()[0].toObject();
    CHECK(host[QStringLiteral("snapshotError")].toString() == QStringLiteral("olderSatellite"));
    CHECK(host[QStringLiteral("version")].toString() == QStringLiteral("2.2.0"));
    const QJsonObject snapshot = host[QStringLiteral("snapshot")].toObject();
    CHECK(snapshot[QStringLiteral("format")].toString() ==
          QStringLiteral("satellite-capabilities"));
    CHECK(snapshot[QStringLiteral("serverVersion")].toString() == QStringLiteral("2.2.0"));
    CHECK(snapshot[QStringLiteral("protocol")].toArray() == QJsonArray{1, 4});
    const QJsonArray backends = snapshot[QStringLiteral("backends")].toArray();
    REQUIRE(backends.size() == 2);
    CHECK(backends[0].toObject()[QStringLiteral("id")].toString() == QStringLiteral("hidmaestro"));
    CHECK_FALSE(backends[1].toObject()[QStringLiteral("available")].toBool());
    const QJsonObject audio = snapshot[QStringLiteral("controllerAudio")].toObject();
    CHECK(audio[QStringLiteral("enabled")].toBool());
    CHECK(audio[QStringLiteral("speaker")].toBool());
    CHECK(snapshot[QStringLiteral("catalogVersion")].toInt() == 2);
}

TEST_CASE("diagnostics bundle: an unreachable Satellite with nothing cached says so and no more",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.hosts[0].snapshotError = SnapshotError::Timeout;
    const QJsonObject host = fixture.parsed()[QStringLiteral("hosts")].toArray()[0].toObject();
    CHECK(host[QStringLiteral("snapshotError")].toString() == QStringLiteral("timeout"));
    CHECK(host[QStringLiteral("snapshot")].isNull());
}

TEST_CASE("diagnostics bundle: a Moonlight host carries its serverinfo words and no name",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    BundleHost moonlight;
    moonlight.kind = "moonlight";
    moonlight.link = "streaming";
    moonlight.grantsKnown = false;
    moonlight.moonlight = dish::composer::BundleMoonlightInfo{"7.1.431.-1", "busy", 1};
    fixture.facts.hosts.push_back(moonlight);
    fixture.facts.hostLabels.push_back(kSecondHost);
    fixture.facts.hostAddresses.push_back("10.0.0.7");
    const std::string bundle = buildDiagnosticsBundle(fixture.facts);
    CHECK(bundle.find(kSecondHost) == std::string::npos);
    CHECK(bundle.find("10.0.0.7") == std::string::npos);
    const QJsonObject host = fixture.parsed()[QStringLiteral("hosts")].toArray()[1].toObject();
    CHECK(host[QStringLiteral("alias")].toString() == QStringLiteral("Host 2"));
    CHECK(host[QStringLiteral("kind")].toString() == QStringLiteral("moonlight"));
    CHECK(host[QStringLiteral("grants")].isNull());
    CHECK(host[QStringLiteral("protocol")].isNull());
    const QJsonObject snapshot = host[QStringLiteral("snapshot")].toObject();
    CHECK(snapshot[QStringLiteral("format")].toString() == QStringLiteral("moonlight-serverinfo"));
    CHECK(snapshot[QStringLiteral("appVersion")].toString() == QStringLiteral("7.1.431.-1"));
    CHECK(snapshot[QStringLiteral("state")].toString() == QStringLiteral("busy"));
    CHECK(snapshot[QStringLiteral("pairStatus")].toInt() == 1);
    CHECK(host[QStringLiteral("snapshotError")].isNull());
}

TEST_CASE("diagnostics bundle: each controller's input and every output lane",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    BundleController& pad = fixture.facts.controllers[0];
    dish::composer::BundleInput input;
    input.frames = 12345;
    input.rateHz = 250;
    input.last = dish::composer::BundleInputState{0x1000, 0, -1, 2, 3, 4, 255, 0x00000001};
    pad.input = input;
    dish::composer::BundleOutput output;
    output.rumble.target = "directUsb";
    output.rumble.sample = {12, dish::diagnostics::packRumble(32768, 0), 1763586000000};
    output.lightbar.target = "directUsb";
    output.lightbar.sample = {2, dish::diagnostics::packRgb(0, 0, 255), 1763586000001};
    output.playerLeds.target = "none";
    output.playerLeds.sample = {1, 0x01, 1763586000002};
    output.triggerEffects.target = "directUsb";
    output.micLed.target = "directUsb";
    output.micLed.sample = {1, 2, 1763586000003};
    output.speaker = dish::composer::BundleVoice{dish::diagnostics::VoiceState::Open, true};
    output.haptics = dish::composer::BundleVoice{dish::diagnostics::VoiceState::Refused, true};
    pad.output = output;

    const QJsonObject parsed =
        fixture.parsed()[QStringLiteral("controllers")].toArray()[0].toObject();
    const QJsonObject in = parsed[QStringLiteral("input")].toObject();
    CHECK(in[QStringLiteral("frames")].toInteger() == 12345);
    CHECK(in[QStringLiteral("rateHz")].toInt() == 250);
    const QJsonObject last = in[QStringLiteral("last")].toObject();
    CHECK(last[QStringLiteral("buttons")].toString() == QStringLiteral("1000"));
    CHECK(last[QStringLiteral("ly")].toInt() == -1);
    CHECK(last[QStringLiteral("rt")].toInt() == 255);
    CHECK(last[QStringLiteral("ext")].toString() == QStringLiteral("00000001"));

    const QJsonObject out = parsed[QStringLiteral("output")].toObject();
    const QJsonObject rumble = out[QStringLiteral("rumble")].toObject();
    CHECK(rumble[QStringLiteral("count")].toInteger() == 12);
    CHECK(rumble[QStringLiteral("target")].toString() == QStringLiteral("directUsb"));
    CHECK(rumble[QStringLiteral("last")].toObject()[QStringLiteral("strong")].toInt() == 32768);
    CHECK(rumble[QStringLiteral("last")].toObject()[QStringLiteral("atMs")].toInteger() ==
          1763586000000);
    CHECK(rumble[QStringLiteral("lastWrite")].toObject()[QStringLiteral("route")].toString() ==
          QStringLiteral("interruptOut"));
    CHECK(out[QStringLiteral("triggerRumble")].isNull());
    CHECK(out[QStringLiteral("lightbar")]
              .toObject()[QStringLiteral("last")]
              .toObject()[QStringLiteral("b")]
              .toInt() == 255);
    CHECK(out[QStringLiteral("playerLeds")]
              .toObject()[QStringLiteral("last")]
              .toObject()[QStringLiteral("mask")]
              .toString() == QStringLiteral("01"));
    const QJsonObject triggers = out[QStringLiteral("triggerEffects")].toObject();
    CHECK(triggers[QStringLiteral("count")].toInteger() == 0);
    CHECK(triggers[QStringLiteral("last")].isNull());
    CHECK(out[QStringLiteral("micLed")]
              .toObject()[QStringLiteral("last")]
              .toObject()[QStringLiteral("state")]
              .toInt() == 2);
    const QJsonObject speaker = out[QStringLiteral("speaker")].toObject();
    CHECK(speaker[QStringLiteral("voice")].toString() == QStringLiteral("open"));
    CHECK(speaker[QStringLiteral("device")].toString() == QStringLiteral("pad"));
    CHECK(speaker[QStringLiteral("frames")].isNull());
    CHECK(out[QStringLiteral("haptics")].toObject()[QStringLiteral("voice")].toString() ==
          QStringLiteral("refused"));
}

TEST_CASE("diagnostics bundle: a path that cannot know says null", "[diagnostics][bundle]") {
    BundleFixture fixture;
    BundleController bare;
    bare.transport = "bluetooth";
    bare.path = "sdl";
    bare.bluetooth = dish::composer::BundleBluetooth{"ble", "le"};
    bare.output = dish::composer::BundleOutput{};
    fixture.facts.controllers = {bare};
    const QJsonObject pad = fixture.parsed()[QStringLiteral("controllers")].toArray()[0].toObject();
    CHECK(pad[QStringLiteral("input")].isNull());
    const QJsonObject out = pad[QStringLiteral("output")].toObject();
    CHECK(out[QStringLiteral("rumble")].toObject()[QStringLiteral("last")].isNull());
    CHECK(out[QStringLiteral("rumble")].toObject()[QStringLiteral("lastWrite")].isNull());
    CHECK(out[QStringLiteral("speaker")].isNull());
    const QJsonObject bluetooth = pad[QStringLiteral("bluetooth")].toObject();
    CHECK(bluetooth[QStringLiteral("profile")].toString() == QStringLiteral("ble"));
    CHECK(bluetooth[QStringLiteral("transportLayer")].toString() == QStringLiteral("le"));
    CHECK(BundleFixture()
              .parsed()[QStringLiteral("controllers")]
              .toArray()[0]
              .toObject()[QStringLiteral("bluetooth")]
              .isNull());
}

TEST_CASE("diagnostics bundle: a Direct pad's hardware from its descriptors, a SDL pad's null",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    BundleController& pad = fixture.facts.controllers.front();
    pad.hardware.bcdDevice = 0x0107;
    dish::diagnostics::UsbInterface hid;
    hid.number = 3;
    hid.interfaceClass = 0x03;
    hid.endpoints = {{0x84, dish::diagnostics::EndpointType::Interrupt, 64, 6}};
    pad.hardware.interfaces = std::vector<dish::diagnostics::UsbInterface>{hid};
    pad.hardware.descriptorSha256 = std::string(64, 'a');
    const QJsonObject hardware = fixture.parsed()[QStringLiteral("controllers")]
                                     .toArray()[0]
                                     .toObject()[QStringLiteral("hardware")]
                                     .toObject();
    CHECK(hardware[QStringLiteral("bcdDevice")].toString() == QStringLiteral("0107"));
    const QJsonObject intf = hardware[QStringLiteral("interfaces")].toArray()[0].toObject();
    CHECK(intf[QStringLiteral("number")].toInt() == 3);
    CHECK(intf[QStringLiteral("class")].toString() == QStringLiteral("03"));
    const QJsonObject ep = intf[QStringLiteral("endpoints")].toArray()[0].toObject();
    CHECK(ep[QStringLiteral("address")].toString() == QStringLiteral("84"));
    CHECK(ep[QStringLiteral("type")].toString() == QStringLiteral("interrupt"));
    CHECK(hardware[QStringLiteral("descriptorSha256")].toString().size() == 64);
    CHECK(hardware[QStringLiteral("firmware")].isNull());

    const QJsonObject bare = BundleFixture()
                                 .parsed()[QStringLiteral("controllers")]
                                 .toArray()[0]
                                 .toObject()[QStringLiteral("hardware")]
                                 .toObject();
    CHECK(bare[QStringLiteral("bcdDevice")].isNull());
    CHECK(bare[QStringLiteral("interfaces")].isNull());
    CHECK(bare[QStringLiteral("descriptorSha256")].isNull());
}

TEST_CASE("diagnostics bundle: a capture rides its controller, and usbHost is what was measured",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    dish::diagnostics::Json capture = dish::diagnostics::Json::object();
    capture.set("format", "usb-descriptors-v1");
    fixture.facts.controllers.front().capture = capture;
    fixture.facts.usbHost = true;
    const QJsonObject parsed = fixture.parsed();
    const QJsonObject pad = parsed[QStringLiteral("controllers")].toArray()[0].toObject();
    CHECK(pad[QStringLiteral("capture")].toObject()[QStringLiteral("format")].toString() ==
          QStringLiteral("usb-descriptors-v1"));
    CHECK(parsed[QStringLiteral("device")].toObject()[QStringLiteral("usbHost")].toBool());
    fixture.facts.usbHost = false;
    const QJsonObject none = fixture.parsed()[QStringLiteral("device")].toObject();
    CHECK(none[QStringLiteral("usbHost")].isBool());
    CHECK_FALSE(none[QStringLiteral("usbHost")].toBool());
}

TEST_CASE("diagnostics bundle: a Bluetooth link nothing could read is null, the profile stays",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    BundleController pad;
    pad.transport = "bluetooth";
    pad.path = "sdl";
    pad.bluetooth = dish::composer::BundleBluetooth{"hid", ""};
    fixture.facts.controllers = {pad};
    const QJsonObject bluetooth = fixture.parsed()[QStringLiteral("controllers")]
                                      .toArray()[0]
                                      .toObject()[QStringLiteral("bluetooth")]
                                      .toObject();
    CHECK(bluetooth[QStringLiteral("profile")].toString() == QStringLiteral("hid"));
    CHECK(bluetooth.contains(QStringLiteral("transportLayer")));
    CHECK(bluetooth[QStringLiteral("transportLayer")].isNull());
}

TEST_CASE("diagnostics bundle: the PC's battery, power saver and thermal state, and no Wi-Fi",
          "[diagnostics][bundle]") {
    BundleFixture fixture;
    fixture.facts.power.battery = dish::diagnostics::BatteryFacts{42, true};
    fixture.facts.power.powerSave = false;
    fixture.facts.thermal = dish::diagnostics::ThermalState::Serious;
    const QJsonObject device = fixture.parsed()[QStringLiteral("device")].toObject();
    const QJsonObject battery = device[QStringLiteral("battery")].toObject();
    CHECK(battery[QStringLiteral("level")].toInt() == 42);
    CHECK(battery[QStringLiteral("charging")].toBool());
    CHECK(device[QStringLiteral("powerSave")].toBool() == false);
    CHECK(device[QStringLiteral("powerSave")].isBool());
    CHECK(device[QStringLiteral("thermal")].toString() == QStringLiteral("serious"));
    CHECK(device[QStringLiteral("wifi")].isNull());
    CHECK(device.contains(QStringLiteral("usbHost")));
    CHECK(device[QStringLiteral("usbHost")].isNull());
    const QJsonObject desktop = BundleFixture().parsed()[QStringLiteral("device")].toObject();
    CHECK(desktop[QStringLiteral("battery")].isNull());
    CHECK(desktop[QStringLiteral("powerSave")].isNull());
    CHECK(desktop[QStringLiteral("thermal")].isNull());
}
