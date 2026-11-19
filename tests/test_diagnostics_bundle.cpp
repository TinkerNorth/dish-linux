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

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cerrno>
#include <initializer_list>
#include <string>
#include <string_view>

using dish::composer::buildDiagnosticsBundle;
using dish::composer::BundleController;
using dish::composer::BundleFacts;
using dish::composer::BundleHost;
using dish::composer::BundleLogEvent;

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
