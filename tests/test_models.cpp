// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Every JSON literal here is a verbatim example from the Protocol-1 contract;
// they are the oracle, so do not "tidy" them.

#include "Models/Models.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace dish::models;
namespace proto = dish::proto;

namespace {
QJsonObject parse(const char* json) { return QJsonDocument::fromJson(QByteArray(json)).object(); }
} // namespace

TEST_CASE("DiscoveredServer::id prefers machineId over ip:port", "[models][identity]") {
    DiscoveredServer s;
    s.ip = "192.168.1.42";
    s.udpPort = 9876;
    s.machineId = "abc123";
    // Protocol-1 keys on machineId so a DHCP address change keeps one identity.
    REQUIRE(s.id() == "mid:abc123");
    REQUIRE(s.isValid());
}

TEST_CASE("DiscoveredServer::id falls back to ip:udpPort without machineId", "[models][identity]") {
    DiscoveredServer s;
    s.ip = "192.168.1.42";
    s.udpPort = 9876;
    REQUIRE(s.id() == "wifi:192.168.1.42:9876");
}

TEST_CASE("DiscoveredServer.fromJson reads machineId from beacon or mDNS mid",
          "[models][identity]") {
    const auto beacon = DiscoveredServer::fromJson(parse(R"({"machineId":"m1"})"));
    REQUIRE(beacon.machineId == "m1");
    const auto mdns = DiscoveredServer::fromJson(parse(R"({"mid":"m2"})"));
    REQUIRE(mdns.machineId == "m2");
}

TEST_CASE("DiscoveredServer.fromJson defaults missing ports", "[models]") {
    const auto s = DiscoveredServer::fromJson(parse(R"({"name":"satellite-1"})"));
    REQUIRE(s.name == "satellite-1");
    REQUIRE(s.ip.isEmpty());
    REQUIRE(s.udpPort == kDefaultUdpPort);
    REQUIRE(s.pairPort == kDefaultPairPort);
    REQUIRE(s.httpPort == kDefaultHttpPort);
}

TEST_CASE("PairResponse parses Path-A success {ok, sharedKey}", "[models][pair]") {
    const auto r = PairResponse::fromJson(parse(
        R"({"ok":true,"message":"paired successfully","sharedKey":"deadbeef","protocolVersion":1})"));
    REQUIRE(r.ok);
    REQUIRE(r.reachable);
    REQUIRE(r.sharedKey.has_value());
    REQUIRE(*r.sharedKey == "deadbeef");
    REQUIRE_FALSE(r.pending);
}

TEST_CASE("PairResponse parses Path-B pending {ok:false, pending:true}", "[models][pair]") {
    const auto r = PairResponse::fromJson(
        parse(R"({"ok":false,"pending":true,"message":"awaiting approval on the satellite"})"));
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.pending);
    REQUIRE(r.reachable);
    REQUIRE_FALSE(r.sharedKey.has_value());
}

TEST_CASE("PairResponse parses bad-pin error body", "[models][pair]") {
    const auto r =
        PairResponse::fromJson(parse(R"({"ok":false,"error":"invalid or expired PIN"})"));
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.has_value());
    REQUIRE(*r.error == "invalid or expired PIN");
}

TEST_CASE("PairResponse::fromStatusJson parses approved with key", "[models][pair]") {
    const auto r = PairResponse::fromStatusJson(
        parse(R"({"ok":true,"status":"approved","sharedKey":"ab12"})"));
    REQUIRE(r.ok);
    REQUIRE(r.status.has_value());
    REQUIRE(*r.status == "approved");
    REQUIRE(*r.sharedKey == "ab12");
}

TEST_CASE("PairResponse::fromStatusJson parses pending/denied", "[models][pair]") {
    REQUIRE(*PairResponse::fromStatusJson(parse(R"({"ok":false,"status":"pending"})")).status ==
            "pending");
    REQUIRE(*PairResponse::fromStatusJson(parse(R"({"ok":false,"status":"denied"})")).status ==
            "denied");
}

TEST_CASE("PairResponse default-constructed is reachable=false", "[models][pair]") {
    PairResponse r;
    REQUIRE_FALSE(r.reachable);
    REQUIRE(r.httpStatus == 0);
}

TEST_CASE("SessionResponse parses the contract's PUT example", "[models][session]") {
    const auto r = SessionResponse::fromJson(parse(R"({
        "connectionId":"conn_ab12cd34",
        "token":"0007a1b2",
        "sessionSalt":"a1b2c3d4e5f60718",
        "epoch":3,
        "maxControllers":16,
        "protocolVersion":1,
        "controllers":[
          {"ctrlIdx":0,"result":"ok","appliedType":0,
           "motion":{"sinkSupportedForType":true,"backendOk":true}}
        ],
        "hostFeatures":{"mouseControl":{"granted":true}}
    })"));
    REQUIRE(r.connectionId.has_value());
    REQUIRE(*r.connectionId == "conn_ab12cd34");
    REQUIRE(*r.token == "0007a1b2");
    REQUIRE(*r.sessionSalt == "a1b2c3d4e5f60718"); // 16 hex = 8 bytes feeds HKDF
    REQUIRE(r.epoch == 3);
    REQUIRE(r.maxControllers == 16);
    REQUIRE(r.controllers.size() == 1);
    const auto& c = r.controllers.first();
    REQUIRE(c.ctrlIdx == 0);
    REQUIRE(c.result == "ok");
    REQUIRE(c.resultCode == proto::kApplyOk);
    REQUIRE(c.ok());
    REQUIRE(c.slotIsLive());
    REQUIRE(c.appliedType == 0);
    REQUIRE(c.motionSinkSupportedForType);
    REQUIRE(c.motionBackendOk);
    REQUIRE(r.mouseControl.granted);
    REQUIRE_FALSE(r.unauthorized());
}

TEST_CASE("SessionResponse maps each per-controller result string to its code",
          "[models][session]") {
    auto one = [](const char* result) {
        return SessionResponse::fromJson(
                   parse((std::string(R"({"controllers":[{"ctrlIdx":0,"result":")") + result +
                          R"("}]})")
                             .c_str()))
            .controllers.first();
    };
    REQUIRE(one("ok").resultCode == proto::kApplyOk);
    REQUIRE(one("noSlots").resultCode == proto::kApplyNoSlots);
    REQUIRE(one("pluginFailed").resultCode == proto::kApplyPluginFailed);
    REQUIRE(one("replugFailed").resultCode == proto::kApplyReplugFailed);
    REQUIRE(one("backendUnavailable").resultCode == proto::kApplyBackendUnavailable);
    REQUIRE(one("invalidType").resultCode == proto::kApplyInvalidType);
    REQUIRE(one("invalidIndex").resultCode == proto::kApplyInvalidIndex);
    // replugFailed leaves the previous pad live; every other non-ok result does
    // not.
    REQUIRE(one("replugFailed").slotIsLive());
    REQUIRE_FALSE(one("noSlots").slotIsLive());
    // A result string from a newer server must land on kApplyUnknown.
    REQUIRE(one("somethingNew").resultCode == proto::kApplyUnknown);
    REQUIRE_FALSE(one("somethingNew").slotIsLive());
}

TEST_CASE("SessionResponse flags the terminal 401 body", "[models][session]") {
    const auto notPaired =
        SessionResponse::fromJson(parse(R"({"error":"unauthorized","code":"NOT_PAIRED"})"));
    REQUIRE(notPaired.unauthorized());
    const auto badProof =
        SessionResponse::fromJson(parse(R"({"error":"unauthorized","code":"BAD_PROOF"})"));
    REQUIRE(badProof.unauthorized());
    REQUIRE_FALSE(SessionResponse::fromJson(parse(R"({"error":"boom"})")).unauthorized());
}

TEST_CASE("SessionResponse parses a host-feature denial with reason", "[models][session]") {
    const auto r = SessionResponse::fromJson(
        parse(R"({"hostFeatures":{"mouseControl":{"granted":false,"reason":"notSupported"}}})"));
    REQUIRE_FALSE(r.mouseControl.granted);
    REQUIRE(r.mouseControl.reason.has_value());
    REQUIRE(*r.mouseControl.reason == "notSupported");
}

TEST_CASE("ControllerPutResponse parses {epoch, controller}", "[models][session]") {
    const auto r = ControllerPutResponse::fromJson(parse(R"({
        "epoch":7,
        "controller":{"ctrlIdx":2,"result":"ok","appliedType":1,
                      "motion":{"sinkSupportedForType":true,"backendOk":false}}
    })"));
    REQUIRE(r.epoch == 7);
    REQUIRE(r.controller.has_value());
    REQUIRE(r.controller->ctrlIdx == 2);
    REQUIRE(r.controller->appliedType == 1);
    REQUIRE(r.controller->ok());
    REQUIRE_FALSE(r.unauthorized());
}

TEST_CASE("SessionViewDto parses the contract's GET example", "[models][session]") {
    const auto v = SessionViewDto::fromJson(parse(R"({
        "connectionId":"conn_ab12cd34",
        "deviceId":"dev",
        "epoch":3,
        "protocolVersion":1,
        "maxControllers":16,
        "controllers":[
          {"ctrlIdx":0,"active":true,"appliedType":0,
           "caps":{"rumble":true,"motion":true,"analogTriggers":true,"lightbar":false},
           "touchpadMode":"off",
           "motion":{"sinkSupportedForType":true,"backendOk":true}}
        ],
        "hostFeatures":{"mouseControl":{"granted":true}}
    })"));
    REQUIRE(*v.connectionId == "conn_ab12cd34");
    REQUIRE(v.epoch == 3);
    REQUIRE(v.protocolVersion == 1);
    REQUIRE(v.maxControllers == 16);
    REQUIRE(v.controllers.size() == 1);
    REQUIRE(v.controllers.first().ctrlIdx == 0);
    REQUIRE(v.controllers.first().active);
    REQUIRE(v.controllers.first().appliedType == 0);
    REQUIRE(v.controllers.first().touchpadMode == "off");
    REQUIRE(v.controllers.first().capsPresent);
    REQUIRE(v.controllers.first().caps ==
            (proto::kCapRumble | proto::kCapMotion | proto::kCapAnalogTriggers));
    REQUIRE(v.controllers.first().motionSinkSupportedForType == true);
    REQUIRE(v.controllers.first().motionBackendOk == true);
    REQUIRE(v.mouseControl.granted);
}

TEST_CASE("SessionViewControllerDto without caps/motion blocks reads as not-reported",
          "[models][session]") {
    // An older server omits both blocks; the parse has to distinguish "not
    // told" from "told all-false" or reconcile fights a server that simply
    // predates the fields.
    const auto v = SessionViewDto::fromJson(parse(R"({
        "connectionId":"c","epoch":1,
        "controllers":[{"ctrlIdx":0,"active":true,"appliedType":1}]
    })"));
    REQUIRE_FALSE(v.controllers.first().capsPresent);
    REQUIRE(v.controllers.first().caps == 0);
    REQUIRE_FALSE(v.controllers.first().motionSinkSupportedForType.has_value());
    REQUIRE_FALSE(v.controllers.first().motionBackendOk.has_value());
}

TEST_CASE("CapabilitiesDto parses the contract's capabilities example", "[models][catalog]") {
    const auto c = CapabilitiesDto::fromJson(parse(R"({
        "protocolVersion":1,
        "serverVersion":"1.6.0",
        "maxControllers":16,
        "backend":{"id":"vigem","supported":true,"available":true,"errorCode":null},
        "motion":{"available":true},
        "host":{
          "catalog":{"supported":true},
          "mouseControl":{"supported":true,"available":true},
          "keyboardControl":{"supported":false},
          "rumble":{"supported":true,"available":true}
        }
    })"));
    REQUIRE(c.serverVersion == "1.6.0");
    REQUIRE(c.maxControllers == 16);
    REQUIRE(c.backendId == "vigem");
    REQUIRE(c.backendSupported);
    REQUIRE(c.backendAvailable);
    REQUIRE_FALSE(c.backendErrorCode.has_value()); // null → unset
    REQUIRE(c.motionAvailable);
    REQUIRE(c.hasHostBlock);
    REQUIRE(c.hostCatalog.supported);
    REQUIRE(c.hostMouseControl.supported);
    REQUIRE(c.hostMouseControl.available == true);
    REQUIRE_FALSE(c.hostKeyboardControl.supported);
    // keyboardControl carries no `available` key at all.
    REQUIRE_FALSE(c.hostKeyboardControl.available.has_value());
    REQUIRE(c.hostRumble.supported);
}

TEST_CASE("CapabilitiesDto without a host block reads hasHostBlock=false", "[models][catalog]") {
    // Contract: absence means "older server, fall back to the defaults", never
    // "a receiver that can do nothing".
    const auto c = CapabilitiesDto::fromJson(
        parse(R"({"backend":{"id":"vigem","supported":true,"available":true}})"));
    REQUIRE_FALSE(c.hasHostBlock);
    REQUIRE_FALSE(c.hostCatalog.supported);
}

TEST_CASE("CapabilitiesDto surfaces a backend errorCode", "[models][catalog]") {
    const auto c = CapabilitiesDto::fromJson(
        parse(R"({"backend":{"id":"vigem","supported":true,"available":false,
                  "errorCode":"DRIVER_MISSING"},"motion":{"available":false}})"));
    REQUIRE_FALSE(c.backendAvailable);
    REQUIRE(c.backendErrorCode.has_value());
    REQUIRE(*c.backendErrorCode == "DRIVER_MISSING");
}

TEST_CASE("CatalogDto parses controllerTypes + hostFeatures from the contract example",
          "[models][catalog]") {
    const auto c = CatalogDto::fromJson(parse(R"({
        "locale":"en",
        "protocolVersion":1,
        "serverVersion":"1.6.0",
        "controllerTypes":[
          {"id":0,"slug":"xbox360","name":"Xbox 360 Controller","shortName":"Xbox",
           "description":"Best compatibility.",
           "image":{"href":"/api/catalog/images/xbox360","etag":"\"1.6.0\""},
           "features":{"rumble":{"supported":true},"motion":{"supported":false}}},
          {"id":1,"slug":"ds4","name":"DualShock 4","shortName":"PlayStation",
           "description":"PlayStation controller.",
           "image":{"href":"/api/catalog/images/ds4","etag":"\"1.6.0\""},
           "features":{"motion":{"supported":true,"requires":"vigembus>=1.17"},
                       "lightbar":{"supported":true}}}
        ],
        "hostFeatures":{"mouseControl":{"supported":true,"modes":["off","ds4","mouse"]}}
    })"));
    REQUIRE(c.locale == "en");
    REQUIRE(c.serverVersion == "1.6.0");
    // No catalogVersion in this payload → the legacy v1 catalog, per contract.
    REQUIRE(c.catalogVersion == 1);
    REQUIRE(c.controllerTypes.size() == 2);
    REQUIRE(c.controllerTypes[0].id == 0);
    REQUIRE(c.controllerTypes[0].slug == "xbox360");
    REQUIRE(c.controllerTypes[0].name == "Xbox 360 Controller");
    REQUIRE(c.controllerTypes[0].imageHref == "/api/catalog/images/xbox360");
    REQUIRE(c.controllerTypes[0].features.value("motion").supported == false);
    REQUIRE(c.controllerTypes[1].slug == "ds4");
    // `requires` is a structured code, not prose.
    REQUIRE(c.controllerTypes[1].features.value("motion").supported);
    REQUIRE(c.controllerTypes[1].features.value("motion").requires_.has_value());
    REQUIRE(*c.controllerTypes[1].features.value("motion").requires_ == "vigembus>=1.17");
    REQUIRE(c.hostFeatures.value("mouseControl").supported);
    REQUIRE(c.hostFeatures.value("mouseControl").modes == QStringList{"off", "ds4", "mouse"});
}

TEST_CASE("CatalogDto parses catalogVersion, per-type feature modes, and emulates",
          "[models][catalog]") {
    const auto c = CatalogDto::fromJson(parse(R"({
        "locale":"en","protocolVersion":1,"serverVersion":"1.6.0","catalogVersion":2,
        "controllerTypes":[
          {"id":1,"slug":"ds4","name":"DualShock 4","shortName":"PlayStation",
           "description":"PlayStation controller.",
           "features":{"touchpad":{"supported":true,"modes":["ds4"]},
                       "rumble":{"supported":true}},
           "emulates":{"sdlType":"ps4","usb":["054C:05C4","054c:09cc"]}}
        ]
    })"));
    REQUIRE(c.catalogVersion == 2);
    const auto& ds4 = c.controllerTypes.first();
    REQUIRE(ds4.features.value("touchpad").modes == QStringList{"ds4"});
    // Absent modes on a feature is a pre-modes catalog, not an empty offer.
    REQUIRE(ds4.features.value("rumble").modes.isEmpty());
    REQUIRE(ds4.emulates.has_value());
    REQUIRE(ds4.emulates->sdlType == "ps4");
    // usb identities normalize to lowercase — SDL/hidapi report either case.
    REQUIRE(ds4.emulates->usb == QStringList{"054c:05c4", "054c:09cc"});
}

TEST_CASE("CatalogTypeDto without emulates reads as no hint", "[models][catalog]") {
    const auto c = CatalogDto::fromJson(parse(R"({
        "controllerTypes":[{"id":0,"slug":"xbox360","name":"X","features":{}}]
    })"));
    REQUIRE_FALSE(c.controllerTypes.first().emulates.has_value());
}

TEST_CASE("ControllerDescriptor.toJson matches the contract's controllers[] element",
          "[models][descriptor]") {
    ControllerDescriptor d;
    d.ctrlIdx = 0;
    d.type = proto::kControllerTypePlayStation;
    d.caps = proto::kCapRumble | proto::kCapMotion | proto::kCapAnalogTriggers; // no lightbar
    d.touchpadMode = proto::kTouchpadModeDs4;
    const auto obj = d.toJson();
    REQUIRE(obj.value("ctrlIdx").toInt() == 0);
    REQUIRE(obj.value("type").toInt() == 1);
    const auto caps = obj.value("caps").toObject();
    REQUIRE(caps.value("rumble").toBool());
    REQUIRE(caps.value("motion").toBool());
    REQUIRE(caps.value("analogTriggers").toBool());
    REQUIRE_FALSE(caps.value("lightbar").toBool());
    REQUIRE(obj.value("touchpadMode").toString() == "ds4");
}

TEST_CASE("controllersJson builds the WHOLE desired array", "[models][descriptor]") {
    ControllerDescriptor a;
    a.ctrlIdx = 0;
    a.touchpadMode = proto::kTouchpadModeOff;
    ControllerDescriptor b;
    b.ctrlIdx = 1;
    b.type = proto::kControllerTypePlayStation;
    b.touchpadMode = proto::kTouchpadModeMouse;
    const auto arr = controllersJson({a, b});
    REQUIRE(arr.size() == 2);
    REQUIRE(arr[0].toObject().value("ctrlIdx").toInt() == 0);
    REQUIRE(arr[0].toObject().value("touchpadMode").toString() == "off");
    REQUIRE(arr[1].toObject().value("ctrlIdx").toInt() == 1);
    REQUIRE(arr[1].toObject().value("touchpadMode").toString() == "mouse");
}

TEST_CASE("RememberedWifi round-trips through JSON list with machineId", "[models]") {
    RememberedWifi r;
    r.id = "mid:abc";
    r.name = "home";
    r.ip = "1.2.3.4";
    r.machineId = "abc";
    const auto back = rememberedListFromJson(rememberedListToJson({r}));
    REQUIRE(back.size() == 1);
    REQUIRE(back.first().id == r.id);
    REQUIRE(back.first().ip == r.ip);
    REQUIRE(back.first().machineId == "abc");
    REQUIRE(back.first().udpPort == kDefaultUdpPort);
}
