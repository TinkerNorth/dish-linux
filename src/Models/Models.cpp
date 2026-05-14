// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Models.h"

namespace dish::models {

namespace {

QString optString(const QJsonObject& obj, const char* key) {
    const auto v = obj.value(QLatin1String(key));
    return v.isString() ? v.toString() : QString();
}

int intOr(const QJsonObject& obj, const char* key, int fallback) {
    const auto v = obj.value(QLatin1String(key));
    if (v.isDouble()) { return v.toInt(fallback); }
    return fallback;
}

} // namespace

QJsonObject DiscoveredServer::toJson() const {
    return QJsonObject{
        {"name", name},         {"ip", ip}, {"udpPort", udpPort}, {"pairPort", pairPort},
        {"httpPort", httpPort},
    };
}

DiscoveredServer DiscoveredServer::fromJson(const QJsonObject& obj) {
    DiscoveredServer s;
    s.name = optString(obj, "name");
    s.ip = optString(obj, "ip");
    s.udpPort = intOr(obj, "udpPort", kDefaultUdpPort);
    s.pairPort = intOr(obj, "pairPort", kDefaultPairPort);
    s.httpPort = intOr(obj, "httpPort", kDefaultHttpPort);
    return s;
}

PairResponse PairResponse::fromJson(const QJsonObject& obj) {
    PairResponse r;
    r.ok = obj.value("ok").toBool(false);
    if (auto e = optString(obj, "error"); !e.isEmpty()) { r.error = e; }
    if (auto k = optString(obj, "sharedKey"); !k.isEmpty()) { r.sharedKey = k; }
    // We got far enough to parse a JSON body, so the server is reachable —
    // even if ok=false. PairingClient sets reachable=false explicitly on
    // every network-level error path.
    r.reachable = true;
    return r;
}

ConnectResponse ConnectResponse::fromJson(const QJsonObject& obj) {
    ConnectResponse r;
    if (auto v = optString(obj, "connectionId"); !v.isEmpty()) { r.connectionId = v; }
    if (auto v = optString(obj, "token"); !v.isEmpty()) { r.token = v; }
    if (auto v = optString(obj, "error"); !v.isEmpty()) { r.error = v; }
    return r;
}

DiscoveredServer RememberedWifi::toDiscovered() const {
    DiscoveredServer s;
    s.name = name;
    s.ip = ip;
    s.udpPort = udpPort;
    s.pairPort = pairPort;
    s.httpPort = httpPort;
    return s;
}

QJsonObject RememberedWifi::toJson() const {
    return QJsonObject{
        {"id", id},           {"name", name},         {"ip", ip},
        {"udpPort", udpPort}, {"pairPort", pairPort}, {"httpPort", httpPort},
    };
}

RememberedWifi RememberedWifi::fromJson(const QJsonObject& obj) {
    RememberedWifi r;
    r.id = optString(obj, "id");
    r.name = optString(obj, "name");
    r.ip = optString(obj, "ip");
    r.udpPort = intOr(obj, "udpPort", kDefaultUdpPort);
    r.pairPort = intOr(obj, "pairPort", kDefaultPairPort);
    r.httpPort = intOr(obj, "httpPort", kDefaultHttpPort);
    return r;
}

QJsonArray rememberedListToJson(const QList<RememberedWifi>& list) {
    QJsonArray arr;
    for (const auto& r : list) { arr.append(r.toJson()); }
    return arr;
}

QList<RememberedWifi> rememberedListFromJson(const QJsonArray& arr) {
    QList<RememberedWifi> out;
    out.reserve(arr.size());
    for (const auto& v : arr) {
        if (v.isObject()) { out.append(RememberedWifi::fromJson(v.toObject())); }
    }
    return out;
}

} // namespace dish::models
