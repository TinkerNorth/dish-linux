// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Models/Models.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

using namespace dish::models;

TEST_CASE("DiscoveredServer::id encodes ip + udp port", "[models]") {
    DiscoveredServer s;
    s.ip = "192.168.1.42";
    s.udpPort = 9876;
    REQUIRE(s.id() == "wifi:192.168.1.42:9876");
    REQUIRE(s.isValid());
}

TEST_CASE("DiscoveredServer.fromJson defaults missing ports", "[models]") {
    QJsonObject obj{{"name", "satellite-1"}};
    const auto s = DiscoveredServer::fromJson(obj);
    REQUIRE(s.name == "satellite-1");
    REQUIRE(s.ip.isEmpty());
    REQUIRE(s.udpPort == kDefaultUdpPort);
    REQUIRE(s.pairPort == kDefaultPairPort);
    REQUIRE(s.httpPort == kDefaultHttpPort);
}

TEST_CASE("DiscoveredServer.toJson / fromJson round-trip", "[models]") {
    DiscoveredServer in;
    in.name = "kitchen";
    in.ip = "10.0.0.5";
    in.udpPort = 1111;
    in.pairPort = 2222;
    in.httpPort = 3333;
    const auto out = DiscoveredServer::fromJson(in.toJson());
    REQUIRE(out.name == in.name);
    REQUIRE(out.ip == in.ip);
    REQUIRE(out.udpPort == in.udpPort);
    REQUIRE(out.pairPort == in.pairPort);
    REQUIRE(out.httpPort == in.httpPort);
}

TEST_CASE("PairResponse parses ok/error/sharedKey", "[models]") {
    const auto good = PairResponse::fromJson(QJsonObject{{"ok", true}, {"sharedKey", "deadbeef"}});
    REQUIRE(good.ok);
    REQUIRE_FALSE(good.error.has_value());
    REQUIRE(good.sharedKey.has_value());
    REQUIRE(*good.sharedKey == "deadbeef");

    const auto bad = PairResponse::fromJson(QJsonObject{{"ok", false}, {"error", "bad pin"}});
    REQUIRE_FALSE(bad.ok);
    REQUIRE(bad.error.has_value());
    REQUIRE(*bad.error == "bad pin");
}

TEST_CASE("SessionResponse parses the protocol-1 PUT response", "[models]") {
    const auto r = SessionResponse::fromJson(QJsonObject{
        {"connectionId", "abc-123"},
        {"token", "0a0b0c0d"},
        {"sessionSalt", "00112233445566aa"},
        {"epoch", 7},
        {"maxControllers", 16},
        {"protocolVersion", 1},
        {"controllers",
         QJsonArray{QJsonObject{
             {"ctrlIdx", 0},
             {"result", "ok"},
             {"appliedType", 1},
             {"motion", QJsonObject{{"sinkSupportedForType", true}, {"backendOk", true}}}}}},
        {"hostFeatures",
         QJsonObject{{"mouseControl", QJsonObject{{"granted", false}, {"reason", "denied"}}}}},
    });
    REQUIRE(r.connectionId.has_value());
    REQUIRE(*r.connectionId == "abc-123");
    REQUIRE(r.token.has_value());
    REQUIRE(*r.token == "0a0b0c0d");
    REQUIRE(r.sessionSalt.has_value());
    REQUIRE(*r.sessionSalt == "00112233445566aa");
    REQUIRE(r.epoch == 7);
    REQUIRE(r.controllers.size() == 1);
    REQUIRE(r.controllers.first().ok());
    REQUIRE(r.controllers.first().slotIsLive());
    REQUIRE(r.controllers.first().appliedType == 1);
    REQUIRE_FALSE(r.mouseControl.granted);
    REQUIRE_FALSE(r.unauthorized());
    REQUIRE_FALSE(r.error.has_value());
}

TEST_CASE("SessionResponse flags terminal 401 codes", "[models]") {
    for (const auto* code : {"NOT_PAIRED", "BAD_PROOF"}) {
        const auto r =
            SessionResponse::fromJson(QJsonObject{{"error", "unauthorized"}, {"code", code}});
        REQUIRE(r.unauthorized());
    }
    const auto other =
        SessionResponse::fromJson(QJsonObject{{"error", "unauthorized"}, {"code", "THROTTLED"}});
    REQUIRE_FALSE(other.unauthorized());
}

TEST_CASE("ControllerApplyDto maps result strings and liveness", "[models]") {
    const auto replug = ControllerApplyDto::fromJson(
        QJsonObject{{"ctrlIdx", 0}, {"result", "replugFailed"}, {"appliedType", 0}});
    REQUIRE_FALSE(replug.ok());
    REQUIRE(replug.slotIsLive()); // previous pad stays plugged; streams keep flowing
    const auto noSlots =
        ControllerApplyDto::fromJson(QJsonObject{{"ctrlIdx", 1}, {"result", "noSlots"}});
    REQUIRE_FALSE(noSlots.ok());
    REQUIRE_FALSE(noSlots.slotIsLive());
    const auto novel =
        ControllerApplyDto::fromJson(QJsonObject{{"ctrlIdx", 2}, {"result", "somethingNew"}});
    REQUIRE(novel.resultCode == dish::proto::kApplyUnknown);
    REQUIRE_FALSE(novel.slotIsLive());
}

TEST_CASE("ControllerDescriptor emits the wire JSON", "[models]") {
    ControllerDescriptor d;
    d.ctrlIdx = 0;
    d.type = dish::proto::kControllerTypePlayStation;
    d.caps = dish::proto::kCapAnalogTriggers | dish::proto::kCapRumble | dish::proto::kCapMotion;
    d.touchpadMode = dish::proto::kTouchpadModeDs4;
    const auto obj = d.toJson();
    REQUIRE(obj.value("ctrlIdx").toInt() == 0);
    REQUIRE(obj.value("type").toInt() == 1);
    REQUIRE(obj.value("touchpadMode").toString() == "ds4");
    const auto arr = controllersJson({d});
    REQUIRE(arr.size() == 1);

    ControllerDescriptor ds;
    ds.type = dish::proto::kControllerTypeDualSense;
    ds.touchpadMode = dish::proto::kTouchpadModeDs4;
    const auto dsObj = ds.toJson();
    REQUIRE(dsObj.value("type").toInt() == 2);
    REQUIRE(dsObj.value("touchpadMode").toString() == "ds4");

    ControllerDescriptor sp;
    sp.type = dish::proto::kControllerTypeSwitchPro;
    sp.touchpadMode = dish::proto::kTouchpadModeOff;
    const auto spObj = sp.toJson();
    REQUIRE(spObj.value("type").toInt() == 3);
    REQUIRE(spObj.value("touchpadMode").toString() == "off");
}

TEST_CASE("ServerCatalog parses controller types and the ds4 touchpad mode", "[models]") {
    const auto touchpadDs4 =
        QJsonObject{{"touchpad", QJsonObject{{"supported", true}, {"modes", QJsonArray{"ds4"}}}}};
    const auto noTouchpad = QJsonObject{{"touchpad", QJsonObject{{"supported", false}}}};
    const auto obj = QJsonObject{
        {"controllerTypes",
         QJsonArray{
             QJsonObject{{"id", 0}, {"slug", "xbox360"}, {"features", noTouchpad}},
             QJsonObject{{"id", 1}, {"slug", "ds4"}, {"features", touchpadDs4}},
             QJsonObject{{"id", 2}, {"slug", "dualsense"}, {"features", touchpadDs4}},
             QJsonObject{{"id", 3}, {"slug", "switchpro"}, {"features", noTouchpad}},
         }},
    };
    const auto cat = ServerCatalog::fromJson(obj);
    REQUIRE(cat.controllerTypes.size() == 4);
    CHECK(cat.controllerTypes[0].id == 0);
    CHECK_FALSE(cat.controllerTypes[0].touchpadDs4); // xbox360: no touch surface
    CHECK(cat.controllerTypes[1].id == 1);
    CHECK(cat.controllerTypes[1].touchpadDs4);
    CHECK(cat.controllerTypes[2].touchpadDs4);
    CHECK_FALSE(cat.controllerTypes[3].touchpadDs4); // switch pro: no touchpad
}

TEST_CASE("ServerCatalog tolerates an emulates block and empty input", "[models]") {
    const auto obj = QJsonObject{
        {"controllerTypes",
         QJsonArray{QJsonObject{
             {"id", 1},
             {"slug", "ds4"},
             // A future physical-pad matcher will read this; the thin client ignores it.
             {"emulates",
              QJsonObject{{"sdlType", "ps4"},
                          {"usb", QJsonArray{QJsonObject{{"vid", 1356}, {"pid", 1476}}}}}},
             {"features", QJsonObject{{"touchpad", QJsonObject{{"supported", true},
                                                               {"modes", QJsonArray{"ds4"}}}}}}}}},
    };
    const auto cat = ServerCatalog::fromJson(obj);
    REQUIRE(cat.controllerTypes.size() == 1);
    CHECK(cat.controllerTypes.first().id == 1);
    CHECK(cat.controllerTypes.first().touchpadDs4);

    CHECK(ServerCatalog::fromJson(QJsonObject{}).isEmpty());
}

TEST_CASE("RememberedWifi round-trips through JSON list", "[models]") {
    RememberedWifi r;
    r.id = "wifi:1.2.3.4:9876";
    r.name = "home";
    r.ip = "1.2.3.4";
    QList<RememberedWifi> list{r};
    const auto arr = rememberedListToJson(list);
    const auto back = rememberedListFromJson(arr);
    REQUIRE(back.size() == 1);
    REQUIRE(back.first().id == r.id);
    REQUIRE(back.first().ip == r.ip);
    REQUIRE(back.first().udpPort == kDefaultUdpPort);
}
