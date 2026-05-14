// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wire-protocol & UI-aggregation DTOs. Field names mirror dish-mac/Models.swift
// and dish-android/Models.kt verbatim so the JSON shape on the wire (and any
// persisted blobs) stay byte-for-byte compatible.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace dish::models {

inline constexpr int kDefaultUdpPort = 9876;
inline constexpr int kDefaultHttpPort = 9877;
inline constexpr int kDefaultPairPort = 9878;

struct DiscoveredServer {
    QString name;
    QString ip;
    int udpPort = kDefaultUdpPort;
    int pairPort = kDefaultPairPort;
    int httpPort = kDefaultHttpPort;

    QString id() const { return QStringLiteral("wifi:%1:%2").arg(ip).arg(udpPort); }
    bool isValid() const { return !ip.isEmpty(); }

    QJsonObject toJson() const;

    // Lenient parse: any missing field falls back to its default — the discovery
    // beacon from the satellite server omits `ip` (the recipient observes it
    // from the packet source). See `satellite/src/net/discovery.cpp`.
    static DiscoveredServer fromJson(const QJsonObject& obj);
};

struct PairResponse {
    bool ok = false;
    std::optional<QString> error;
    std::optional<QString> sharedKey;
    // True iff we received any JSON body from the server. False for synthesized
    // failure responses (socket / connect / send errors). Not on the wire —
    // the server never sends this field; it's set client-side by
    // `PairingClient::pair` so the manager can distinguish "moved networks"
    // from "needs PIN". Mirrors dish-mac PairResponse.reachable.
    bool reachable = false;

    static PairResponse fromJson(const QJsonObject& obj);
};

struct ConnectResponse {
    std::optional<QString> connectionId;
    std::optional<QString> token;
    std::optional<QString> error;

    static ConnectResponse fromJson(const QJsonObject& obj);
};

enum class ConnectionLive { Idle, Connecting, Connected };

struct ConnectionSummary {
    QString id;
    QString label;
    QString detail;
    ConnectionLive live = ConnectionLive::Idle;
    std::optional<QString> boundSlotId;
};

enum class SlotInputType { Virtual, Physical };

struct ControllerSlot {
    QString id;
    SlotInputType inputType = SlotInputType::Virtual;
    QString name;
    QString physicalDeviceId;
    std::optional<QString> boundConnectionId;
    std::optional<ConnectionSummary> boundStatus;
};

inline constexpr const char* kVirtualSlotId = "virtual";

struct RememberedWifi {
    QString id;
    QString name;
    QString ip;
    int udpPort = kDefaultUdpPort;
    int pairPort = kDefaultPairPort;
    int httpPort = kDefaultHttpPort;

    DiscoveredServer toDiscovered() const;
    QJsonObject toJson() const;
    static RememberedWifi fromJson(const QJsonObject& obj);
};

QJsonArray rememberedListToJson(const QList<RememberedWifi>& list);
QList<RememberedWifi> rememberedListFromJson(const QJsonArray& arr);

} // namespace dish::models
