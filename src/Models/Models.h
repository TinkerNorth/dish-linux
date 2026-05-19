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

#include <cstdint>
#include <optional>

namespace dish::models {

inline constexpr int kDefaultUdpPort = 9876;
// The satellite's client-facing API moved to HTTPS on a single port (9443).
// Both the connection API and pairing now ride that one HTTPS server, so the
// HTTP and pair ports collapse to the same value. Kept as two named constants
// so existing call sites (httpPort / pairPort) stay readable.
inline constexpr int kDefaultHttpPort = 9443;
inline constexpr int kDefaultPairPort = 9443;

// Which discovery path surfaced a satellite. mDNS / Bonjour is the modern
// path; Broadcast is the legacy UDP beacon; Both means it answered on each.
// Not a wire field — assigned client-side by the discovery merge.
enum class DiscoverySource { Broadcast, Mdns, Both };

// Short human label for the connections list.
inline QString discoverySourceLabel(DiscoverySource source) {
    switch (source) {
    case DiscoverySource::Broadcast:
        return QStringLiteral("UDP broadcast");
    case DiscoverySource::Mdns:
        return QStringLiteral("mDNS");
    case DiscoverySource::Both:
        return QStringLiteral("mDNS + broadcast");
    }
    return {};
}

struct DiscoveredServer {
    QString name;
    QString ip;
    int udpPort = kDefaultUdpPort;
    int pairPort = kDefaultPairPort;
    int httpPort = kDefaultHttpPort;
    // Discovery path this server was heard on. Not serialised — `toJson` /
    // `fromJson` omit it, so a decoded beacon keeps the Broadcast default.
    DiscoverySource source = DiscoverySource::Broadcast;

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

// What a physical controller's hardware exposes, detected once at attach.
// Mirrors dish-mac's ControllerCapabilities. Distinct from any user setting
// for whether a feature is forwarded — a chip being present means "this
// controller has the hardware". dish-linux currently forwards every detected
// capability unconditionally (no per-feature on/off setting), so `hasMotion`
// alone is enough for the UI to tell "no gyro" apart from "gyro present".
struct ControllerCapabilities {
    bool hasMotion = false;

    // True iff SDL reported an addressable RGB LED for the device
    // (SDL_GameControllerHasLED) — DualSense / DualShock 4. Drives the slot
    // card's lightbar chip and the CAP_LIGHTBAR advertisement.
    bool hasLightbar = false;

    // Most recent battery sample for the pad — the same (level, status) pair
    // forwarded on MSG_BATTERY. For a wireless pad this is the controller's
    // own charge; for a wired/unknown pad it is the host machine's battery
    // (the laptop's percentage, or 100 % / WIRED on a desktop). The slot card
    // renders it as a battery chip. `batteryLevel` is 0..100 percent or 0xFF
    // (unknown); `batteryStatus` is a SatelliteClient::kBatteryStatus*
    // constant. 0xFF / 0 until the first 30 s poll completes.
    std::uint8_t batteryLevel = 0xFF;
    std::uint8_t batteryStatus = 0;
};

struct ControllerSlot {
    QString id;
    QString name;
    QString physicalDeviceId;
    std::optional<QString> boundConnectionId;
    std::optional<ConnectionSummary> boundStatus;
    // Hardware capabilities detected at attach (see SDLGamepadBridge).
    ControllerCapabilities capabilities;
};

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
