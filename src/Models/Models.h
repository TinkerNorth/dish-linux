// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wire-protocol & UI-aggregation DTOs. Field names mirror dish-mac/Models.swift
// and dish-android/Models.kt verbatim so the JSON shape on the wire (and any
// persisted blobs) stay byte-for-byte compatible.

#pragma once

#include "Models/Protocol.h"

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
enum class DiscoverySource : std::uint8_t { Broadcast, Mdns, Both };

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
    // Stable per-install satellite identity from the beacon ("machineId") /
    // mDNS TXT ("mid"). Empty for satellites that predate it. Protocol-1 keys
    // remembered satellites on this (never ip/port) — see `id()`.
    QString machineId;
    // Discovery path this server was heard on. Not serialised — `toJson` /
    // `fromJson` omit it, so a decoded beacon keeps the Broadcast default.
    DiscoverySource source = DiscoverySource::Broadcast;

    // The stable identity a dish keys a satellite on. Prefers `machineId`
    // (survives DHCP address changes), falls back to ip:udpPort for older
    // satellites that don't advertise one. Both discovery paths and the
    // remembered store key on this, so one physical receiver collapses to a
    // single entry instead of one row per IP. Mirrors dish-windows
    // DiscoveredServer::id / dish-android stableKey.
    QString id() const {
        if (!machineId.isEmpty()) { return QStringLiteral("mid:%1").arg(machineId); }
        return QStringLiteral("wifi:%1:%2").arg(ip).arg(udpPort);
    }
    bool isValid() const { return !ip.isEmpty(); }

    QJsonObject toJson() const;

    // Lenient parse: any missing field falls back to its default — the discovery
    // beacon from the satellite server omits `ip` (the recipient observes it
    // from the packet source). See `satellite/src/net/discovery.cpp`.
    static DiscoveredServer fromJson(const QJsonObject& obj);
};

struct PairResponse {
    bool ok = false;
    bool pending = false; // Path B: awaiting operator approval on the satellite
    std::optional<QString> error;
    std::optional<QString> sharedKey;
    int protocolVersion = proto::kProtocolVersion;
    // HTTP status of the exchange (0 = transport never produced a response).
    // Lets the manager spot a 409 version mismatch without re-reading the body.
    int httpStatus = 0;
    // True iff we received any JSON body from the server. False for synthesized
    // failure responses (socket / connect / send errors). Not on the wire —
    // the server never sends this field; it's set client-side by
    // `PairingClient::pair` so the manager can distinguish "moved networks"
    // from "needs PIN". Mirrors dish-mac PairResponse.reachable.
    bool reachable = false;

    static PairResponse fromJson(const QJsonObject& obj);
};

// ── Protocol-1 REST control-plane DTOs (contract §Session / §Controller) ────
// Field names mirror dish-windows / dish-android verbatim so the JSON on the
// wire stays byte-for-byte compatible.

// One controller's apply outcome inside a session/controller PUT response.
// `result` is the protocol string (never localized); `resultCode` is its
// proto::kApply* mapping. `motion*` mirror the response's motion sub-object.
struct ControllerApplyDto {
    int ctrlIdx = 0;
    QString result;
    std::uint8_t resultCode = proto::kApplyUnknown;
    int appliedType = proto::kControllerTypeXbox;
    bool motionSinkSupportedForType = false;
    bool motionBackendOk = false;

    bool ok() const { return resultCode == proto::kApplyOk; }
    // replugFailed leaves the PREVIOUS pad live (appliedType reports it):
    // streams keep flowing rather than killing a working pad.
    bool slotIsLive() const { return proto::applyResultSlotIsLive(resultCode); }

    static ControllerApplyDto fromJson(const QJsonObject& obj);
};

// Host-feature grant (server policy, returned in the PUT/GET response).
struct HostFeatureGrant {
    bool granted = false;
    std::optional<QString> reason; // notSupported|backendUnavailable|denied, when !granted

    static HostFeatureGrant fromJson(const QJsonObject& obj);
};

// PUT /api/connections response. Also doubles as the error body (error/code).
// `sessionSalt` (16-hex → 8 bytes) + `token` feed deriveSessionKey; missing
// `sessionSalt` means the key can't be derived (a pre-protocol-1 server).
struct SessionResponse {
    std::optional<QString> connectionId;
    std::optional<QString> token;       // 8-hex (4 bytes BE)
    std::optional<QString> sessionSalt; // 16-hex (8 bytes)
    int epoch = 0;
    int maxControllers = 16;
    int protocolVersion = proto::kProtocolVersion;
    QList<ControllerApplyDto> controllers;
    HostFeatureGrant mouseControl;
    std::optional<QString> error;
    // Machine-readable 401 cause: NOT_PAIRED | BAD_PROOF. Either is terminal.
    std::optional<QString> code;
    int httpStatus = 0;
    bool reachable = false;

    bool unauthorized() const {
        return code.has_value() && (*code == QLatin1String(proto::kAuthCodeNotPaired.data()) ||
                                    *code == QLatin1String(proto::kAuthCodeBadProof.data()));
    }

    static SessionResponse fromJson(const QJsonObject& obj);
};

// PUT /api/connections/{id}/controllers/{idx} response: one controller's apply
// result + the session epoch (no token rotation on the per-controller route).
struct ControllerPutResponse {
    int epoch = 0;
    std::optional<ControllerApplyDto> controller;
    std::optional<QString> error;
    std::optional<QString> code;
    int httpStatus = 0;
    bool reachable = false;

    bool unauthorized() const {
        return code.has_value() && (*code == QLatin1String(proto::kAuthCodeNotPaired.data()) ||
                                    *code == QLatin1String(proto::kAuthCodeBadProof.data()));
    }

    static ControllerPutResponse fromJson(const QJsonObject& obj);
};

// One applied controller from GET /api/connections/{id} (the reconcile view).
struct SessionViewControllerDto {
    int ctrlIdx = 0;
    bool active = false;
    int appliedType = proto::kControllerTypeXbox;
    QString touchpadMode;

    static SessionViewControllerDto fromJson(const QJsonObject& obj);
};

// GET /api/connections/{id}: the reconcile endpoint's applied state + epoch.
struct SessionViewDto {
    std::optional<QString> connectionId;
    int epoch = 0;
    QList<SessionViewControllerDto> controllers;
    HostFeatureGrant mouseControl;
    std::optional<QString> error;
    std::optional<QString> code;
    int httpStatus = 0;
    bool reachable = false;

    bool unauthorized() const {
        return code.has_value() && (*code == QLatin1String(proto::kAuthCodeNotPaired.data()) ||
                                    *code == QLatin1String(proto::kAuthCodeBadProof.data()));
    }

    static SessionViewDto fromJson(const QJsonObject& obj);
};

// Declarative per-controller desired state sent in the session/controller PUT
// body. Always sent WHOLE (a toggle = re-send with one field changed); the
// server converges. Owns its own JSON so the request shape is unit-testable
// without a socket. Mirrors dish-windows models::ControllerDescriptor.
struct ControllerDescriptor {
    int ctrlIdx = 0;
    std::uint8_t type = proto::kControllerTypeXbox;
    std::uint16_t caps = 0; // proto::kCap* word
    std::uint8_t touchpadMode = proto::kTouchpadModeOff;

    // The single-descriptor JSON object (one element of the controllers[]
    // array, and the per-controller PUT body). `ctrlIdx` is included; on the
    // per-controller route the path's index wins server-side anyway.
    QJsonObject toJson() const;
};

// Build the controllers[] array JSON from a desired descriptor list.
QJsonArray controllersJson(const QList<ControllerDescriptor>& descriptors);

// UI-facing link state for one connection. This is the chip a row renders;
// combines the persistent "Pairing" axis (have we paired?) and the live
// "Presence" axis (do we see it / is the session up?).
//
// Internally a Satellite session also has [net::SessionState] (the wire-level
// presence axis only); [LinkState] is derived from that plus discovery /
// remembered presence in [ConnectionHub::rebuild].
//
// | LinkState  | Pairing axis    | Presence axis    | User-facing chip |
// |------------|-----------------|------------------|------------------|
// | Found      | unpaired        | seen             | "Found"          |
// | Stale      | broken (lost)   | any              | "Needs pairing"  |
// | Saved      | paired          | absent           | "Offline"        |
// | Ready      | paired          | seen, no session | "Ready"          |
// | Connecting | paired          | linking          | "Connecting…"    |
// | Connected  | paired          | live             | "Online"         |
// | Unstable   | paired          | faltering        | "Unsteady"       |
//
// **Stale** is now reachable: a terminal 401 (NOT_PAIRED/BAD_PROOF) or a
// close-notify(unpaired) drops the key and parks the row here so the chip
// reads "Needs pairing" and auto-retry stops.
//
// **Unstable** is now reachable: the alive-poll reads the client's
// consecutive-missed-ack count and flips Connected → Unstable at 2 misses
// ("not responding" per contract §Liveness), back on the next ack.
enum class LinkState : std::uint8_t { Found, Stale, Saved, Ready, Connecting, Connected, Unstable };

struct ConnectionSummary {
    QString id;
    QString label;
    QString detail;
    LinkState live = LinkState::Saved;
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
    // Persisted machineId so a remembered satellite that changes IP keeps its
    // identity (the `id` is already the machineId-preferring stable key).
    // Empty for rows persisted before protocol-1 — they still load.
    QString machineId;

    DiscoveredServer toDiscovered() const;
    QJsonObject toJson() const;
    static RememberedWifi fromJson(const QJsonObject& obj);

    bool operator==(const RememberedWifi& o) const {
        return id == o.id && name == o.name && ip == o.ip && udpPort == o.udpPort &&
               pairPort == o.pairPort && httpPort == o.httpPort && machineId == o.machineId;
    }
    bool operator!=(const RememberedWifi& o) const { return !(*this == o); }
};

QJsonArray rememberedListToJson(const QList<RememberedWifi>& list);
QList<RememberedWifi> rememberedListFromJson(const QJsonArray& arr);

} // namespace dish::models
