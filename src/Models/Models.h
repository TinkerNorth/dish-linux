// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wire-protocol and UI-aggregation DTOs for protocol-1 (satellite/docs/contract.md).
// The REST shapes mirror dish-android's Models.kt verbatim so the JSON on the
// wire and any persisted blobs stay byte-for-byte compatible. The Qt-free
// protocol constants live in core/model/Protocol.h.

#pragma once

#include "core/model/Protocol.h"
#include "core/reducer/DirectClaimFailure.h"
#include "core/reducer/PathChoice.h"
#include "core/reducer/UsbPathMachine.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <string_view>

namespace dish::models {

inline constexpr int kDefaultUdpPort = 9876;
// The connection API and pairing share one HTTPS port, so both constants
// resolve to the same value; discovery still advertises them separately.
inline constexpr int kDefaultHttpPort = 9443;
inline constexpr int kDefaultPairPort = 9443;

// Not a wire field — assigned client-side by the discovery merge.
enum class DiscoverySource : std::uint8_t { Broadcast, Mdns, Both };

// Routed through translate() even though these are protocol acronyms, so the
// i18n pipeline stays complete and a translator can override.
inline QString discoverySourceLabel(DiscoverySource source) {
    constexpr const char* ctx = "dish::models::DiscoverySource";
    switch (source) {
    case DiscoverySource::Broadcast:
        return QCoreApplication::translate(ctx, "UDP broadcast");
    case DiscoverySource::Mdns:
        return QCoreApplication::translate(ctx, "mDNS");
    case DiscoverySource::Both:
        return QCoreApplication::translate(ctx, "mDNS + broadcast");
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
    // mDNS TXT ("mid"). Empty for satellites that predate it.
    QString machineId;
    // Not serialised: `toJson`/`fromJson` omit it, so a decoded beacon keeps
    // the Broadcast default.
    DiscoverySource source = DiscoverySource::Broadcast;

    // Prefers `machineId` so a DHCP address change doesn't fork the entry;
    // falls back to ip:udpPort for satellites that don't advertise one. Both
    // discovery paths and the remembered store key on this, collapsing one
    // physical receiver to a single row instead of one per IP.
    QString id() const {
        if (!machineId.isEmpty()) { return QStringLiteral("mid:%1").arg(machineId); }
        return QStringLiteral("wifi:%1:%2").arg(ip).arg(udpPort);
    }
    bool isValid() const { return !ip.isEmpty(); }

    QJsonObject toJson() const;

    // Lenient: the beacon omits `ip`, since the recipient observes it from the
    // packet source.
    static DiscoveredServer fromJson(const QJsonObject& obj);
};

// POST /api/pair. The PIN paths always answer HTTP 200, so the dish classifies
// on ok/pending. `reachable` is NOT on the wire: it is set client-side, true
// once a JSON body parses and false on a synthesised network-error response.
struct PairResponse {
    bool ok = false;
    bool pending = false;          // Path B: awaiting operator approval
    std::optional<QString> status; // /api/pair/status: approved|pending|denied|none
    std::optional<QString> error;
    std::optional<QString> sharedKey;
    int protocolVersion = proto::kProtocolVersion;
    // 0 = the transport never produced a response. Lets the manager spot a 409
    // version mismatch without re-reading the body.
    int httpStatus = 0;
    bool reachable = false;

    static PairResponse fromJson(const QJsonObject& obj);
    static PairResponse fromStatusJson(const QJsonObject& obj);
};

// One controller's apply outcome inside a session/controller PUT response.
// `result` is the protocol string, never localized.
struct ControllerApplyDto {
    int ctrlIdx = 0;
    QString result;
    std::uint8_t resultCode = proto::kApplyUnknown;
    int appliedType = proto::kControllerTypeXbox;
    bool motionSinkSupportedForType = false;
    bool motionBackendOk = false;

    bool ok() const { return resultCode == proto::kApplyOk; }
    // replugFailed leaves the PREVIOUS pad live, so streams keep flowing rather
    // than killing a working pad.
    bool slotIsLive() const { return proto::applyResultSlotIsLive(resultCode); }

    static ControllerApplyDto fromJson(const QJsonObject& obj);
};

// Server policy, returned in the PUT/GET response.
// The one place the auth-failure codes are compared. Three response types carry
// the same `code` field and asked the same question three times.
//
// Sized construction, not `QLatin1String(sv.data())`: a string_view carries no
// null-termination guarantee, and the single-argument overload would run off
// the end of a view that is ever built from anything but a literal.
inline bool isAuthFailureCode(const QString& code) {
    const auto latin1 = [](std::string_view sv) {
        return QLatin1String(sv.data(), static_cast<qsizetype>(sv.size()));
    };
    return code == latin1(proto::kAuthCodeNotPaired) || code == latin1(proto::kAuthCodeBadProof);
}

struct HostFeatureGrant {
    bool granted = false;
    std::optional<QString> reason; // notSupported|backendUnavailable|denied, when !granted

    static HostFeatureGrant fromJson(const QJsonObject& obj);
};

// PUT /api/connections response; doubles as the error body. `sessionSalt` and
// `token` feed deriveSessionKey — a missing salt means a pre-protocol-1 server
// and no derivable key.
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
    // 401 cause: NOT_PAIRED | BAD_PROOF. Either is terminal.
    std::optional<QString> code;
    int httpStatus = 0;
    bool reachable = false;

    bool unauthorized() const { return code.has_value() && isAuthFailureCode(*code); }

    static SessionResponse fromJson(const QJsonObject& obj);
};

// PUT /api/connections/{id}/controllers/{idx}. No token rotation on this route.
struct ControllerPutResponse {
    int epoch = 0;
    std::optional<ControllerApplyDto> controller;
    std::optional<QString> error;
    std::optional<QString> code;
    int httpStatus = 0;
    bool reachable = false;

    bool unauthorized() const { return code.has_value() && isAuthFailureCode(*code); }

    static ControllerPutResponse fromJson(const QJsonObject& obj);
};

// One applied controller from GET /api/connections/{id}.
struct SessionViewControllerDto {
    int ctrlIdx = 0;
    bool active = false;
    int appliedType = proto::kControllerTypeXbox;
    QString touchpadMode;
    // `capsPresent` distinguishes "the server omitted the block" from an
    // all-false fold, so reconcile only compares caps it was actually told.
    std::uint16_t caps = 0;
    bool capsPresent = false;
    // Absent on older satellites.
    std::optional<bool> motionSinkSupportedForType;
    std::optional<bool> motionBackendOk;

    static SessionViewControllerDto fromJson(const QJsonObject& obj);
};

// GET /api/connections/{id}: the reconcile endpoint's applied state.
struct SessionViewDto {
    std::optional<QString> connectionId;
    int epoch = 0;
    int protocolVersion = proto::kProtocolVersion;
    int maxControllers = 16;
    QList<SessionViewControllerDto> controllers;
    HostFeatureGrant mouseControl;
    std::optional<QString> error;
    std::optional<QString> code;
    int httpStatus = 0;
    bool reachable = false;

    bool unauthorized() const { return code.has_value() && isAuthFailureCode(*code); }

    static SessionViewDto fromJson(const QJsonObject& obj);
};

// The receiver's own inventory for a feature slug. `supported` is the static
// fact; `available` is a coarse runtime read, absent when unreported.
struct HostCapabilityDto {
    bool supported = false;
    std::optional<bool> available;
};

// GET /api/server/capabilities: live backend health. Unauthenticated, so it is
// the only pre-pairing signal that the receiver's driver stack is broken.
struct CapabilitiesDto {
    int protocolVersion = proto::kProtocolVersion;
    QString serverVersion;
    int maxControllers = 16;
    QString backendId;
    bool backendSupported = false;
    bool backendAvailable = false;
    std::optional<QString> backendErrorCode;
    bool motionAvailable = false;
    // An older satellite omits the `host` block entirely, so the client must
    // fall back to catalog-era defaults rather than read four false entries as
    // "the receiver can do nothing".
    bool hasHostBlock = false;
    HostCapabilityDto hostCatalog;
    HostCapabilityDto hostMouseControl;
    HostCapabilityDto hostKeyboardControl;
    HostCapabilityDto hostRumble;
    int httpStatus = 0;
    bool reachable = false;

    static CapabilitiesDto fromJson(const QJsonObject& obj);
};

// GET /api/catalog sub-DTOs — the localized controller-type catalog behind the
// Emulate picker. Feature slugs are capability data the client only offers when
// it has code for them.
struct CatalogFeatureDto {
    bool supported = false;
    std::optional<QString> requires_; // structured code e.g. "vigembus>=1.17"
    // Empty means a pre-modes catalog: the client falls back to its prior
    // assumption rather than gating the feature off.
    QStringList modes;
};

// Optional hint for which detected pad a virtual type is the natural default
// for. The mapping policy lives on the server so new hardware needs no client
// release.
struct CatalogEmulatesDto {
    QString sdlType; // clients' SDL_GameControllerType vocabulary (ps4, ps5, …)
    QStringList usb; // lowercase "vid:pid" identities
};

struct CatalogTypeDto {
    int id = 0;
    QString slug;
    QString name;
    QString shortName;
    QString description;
    QString imageHref;
    QString imageEtag;
    // Keys are protocol constants (rumble, motion, …).
    QHash<QString, CatalogFeatureDto> features;
    std::optional<CatalogEmulatesDto> emulates;

    static CatalogTypeDto fromJson(const QJsonObject& obj);
};

struct CatalogHostFeatureDto {
    bool supported = false;
    QStringList modes; // valid descriptor touchpadMode values for this host feature
};

struct CatalogDto {
    QString locale;
    int protocolVersion = proto::kProtocolVersion;
    // Catalog SCHEMA version, distinct from the wire protocol and the build. A
    // response omitting it is the legacy v1 catalog, so absent reads as 1 and
    // clients branch on schema level instead of sniffing for fields.
    int catalogVersion = 1;
    QString serverVersion;
    QString etag; // "<serverVersion>+<locale>" — cache key for If-None-Match
    QList<CatalogTypeDto> controllerTypes;
    QHash<QString, CatalogHostFeatureDto> hostFeatures;
    int httpStatus = 0;
    bool notModified = false; // 304 → caller serves its cache
    bool reachable = false;

    static CatalogDto fromJson(const QJsonObject& obj);
};

// Declarative per-controller desired state for the session/controller PUT body.
// Always sent WHOLE — a toggle is a re-send with one field changed, and the
// server converges.
struct ControllerDescriptor {
    int ctrlIdx = 0;
    std::uint8_t type = proto::kControllerTypeXbox;
    std::uint16_t caps = 0; // proto::kCap* word
    std::uint8_t touchpadMode = proto::kTouchpadModeOff;

    // `ctrlIdx` is included, though on the per-controller route the path's
    // index wins server-side anyway.
    QJsonObject toJson() const;
};

QJsonArray controllersJson(const QList<ControllerDescriptor>& descriptors);

// The chip a connection row renders: the persistent "have we paired?" axis
// crossed with the live "do we see it / is the session up?" axis.
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
// Stale is entered by a terminal 401 or a close-notify(unpaired), which drops
// the key and stops auto-retry. Unstable enters at two consecutive missed
// heartbeat acks (the contract's "not responding" threshold) and recovers to
// Connected the moment an ack lands.
enum class LinkState : std::uint8_t { Found, Stale, Saved, Ready, Connecting, Connected, Unstable };

// What a physical controller's HARDWARE exposes, detected once at attach.
// Distinct from any user "forward this feature?" preference, so the slot card
// can tell "my pad has no gyro" apart from "gyro is switched off".
struct ControllerCapabilities {
    bool hasMotion = false;
    bool hasLightbar = false;
    // Gates both the Touchpad and the Mouse rows of the capability solver:
    // mouse is a routing of the touchpad, so a pad without one can't drive it.
    bool hasTouchpad = false;
    // Whether the pad carries rumble motors at all: the SDL probe for a
    // Standard slot, the parser family for a USB-direct claim. The wire fold
    // additionally requires a path that can drive them (CAP_RUMBLE is never
    // advertised for a Direct claim — no output write path exists yet).
    bool hasRumble = false;

    // For a wireless pad this is the controller's own charge; for a wired or
    // unknown one it is the HOST machine's battery. `batteryLevel` is 0..100 or
    // 0xFF for unknown, `batteryStatus` a SatelliteClient::kBatteryStatus*
    // constant. 0xFF / 0 until the first 30 s poll completes.
    std::uint8_t batteryLevel = 0xFF;
    std::uint8_t batteryStatus = 0;
};

struct ConnectionSummary {
    QString id;
    QString label;
    QString detail;
    LinkState live = LinkState::Saved;
    std::optional<QString> boundSlotId;
};

// The live-stats numbers the slot card shows. `*Hz` are the current quantized
// rates, `*PeakHz` the high-water marks shown with a "~" when the live value is
// idle, `directPollHz` the independently-measured USB-direct poll rate (0 for a
// non-direct pad). All 0 until a reading lands; `hasAny()` gates the row.
struct SlotLiveRates {
    int gamepadHz = 0;
    int gamepadPeakHz = 0;
    int motionHz = 0;
    int motionPeakHz = 0;
    int directPollHz = 0;

    bool hasAny() const {
        return gamepadHz > 0 || gamepadPeakHz > 0 || motionHz > 0 || motionPeakHz > 0 ||
               directPollHz > 0;
    }
    bool operator==(const SlotLiveRates& o) const {
        return gamepadHz == o.gamepadHz && gamepadPeakHz == o.gamepadPeakHz &&
               motionHz == o.motionHz && motionPeakHz == o.motionPeakHz &&
               directPollHz == o.directPollHz;
    }
    bool operator!=(const SlotLiveRates& o) const { return !(*this == o); }
};

// The desktop clients expose no virtual touch-controller slot, so android's SlotInputType
// and physicalDeviceId have no counterpart here.
struct ControllerSlot {
    QString id;
    QString name;
    std::optional<QString> boundConnectionId;
    std::optional<ConnectionSummary> boundStatus;
    // The resolved type's localized short name for the "· as <type>" suffix.
    // Empty when unbound or the catalog offers no name, and the row then omits
    // the suffix rather than guessing.
    QString emulateName;
    // SDL saw the device but the slot hasn't settled; the card renders the busy
    // state instead of chips and actions.
    bool registering = false;
    ControllerCapabilities capabilities;
    // A USB-direct (raw-HID) synthetic rather than an SDL pad. Direct streams
    // continuously, so its gamepad Hz reads live rather than as a "~peak".
    bool usbDirect = false;
    // Attached over Bluetooth. reducer::slotPathFields refuses USB-path fields
    // for a BT slot and twin-dedup hides BT twins last — the raw-HID claim is
    // USB-only, so a BT instance is never the claimed device. Always false for
    // a USB-direct synthetic.
    bool bluetooth = false;
    // A raw-joystick-backed SDL pad the "Configure controls" page may remap.
    // False for synthetics and SDL-recognised game controllers, which use SDL's
    // own mapping and ignore any remap, so the page entry must not show.
    bool remappable = false;
    SlotLiveRates liveRates;

    // Stamped in AppModel::rebuild() from the matching UsbController via
    // reducer::slotPathFields. `pathSupported` is false for a pad the raw-HID
    // gateway can't claim (an Xbox pad, which publishes no hidraw node).
    // Defaults are the inert "no controller" state.
    reducer::UsbPhase pathPhase = reducer::UsbPhase::Routed;
    reducer::PathChoice desiredPath = reducer::PathChoice::Standard;
    bool pathSupported = false;
    std::optional<reducer::DirectClaimFailure> directFailure;

    // The raw-HID fast lane knows this model's report layout. Drives the
    // "Layout guessed" warn chip, which belongs on the Direct option card
    // because the risk exists only there. Always false for a Bluetooth pad.
    bool verifiedModel = false;
};

struct RememberedWifi {
    QString id;
    QString name;
    QString ip;
    int udpPort = kDefaultUdpPort;
    int pairPort = kDefaultPairPort;
    int httpPort = kDefaultHttpPort;
    // Persisted so a remembered satellite that changes IP keeps its identity.
    QString machineId;

    DiscoveredServer toDiscovered() const;
    QJsonObject toJson() const;
    static RememberedWifi fromJson(const QJsonObject& obj);
};

QJsonArray rememberedListToJson(const QList<RememberedWifi>& list);
QList<RememberedWifi> rememberedListFromJson(const QJsonArray& arr);

// A typed user-facing notification. `kind` is a free-form tag for callers that
// want to dedup or categorise; the renderer does not switch on it. A persistent
// banner the user can't dismiss sets `dismissible` false.
struct DishNotification {
    enum class Severity : std::uint8_t { Info, Success, Warn, Error };

    // kDurationPersistent keeps the toast up until NotificationQueue::dismiss.
    static constexpr int kDurationShortMs = 3'500;
    static constexpr int kDurationLongMs = 6'000;
    static constexpr int kDurationPersistent = 0;

    int id = 0;
    QString kind;
    Severity severity = Severity::Info;
    QString message;
    bool dismissible = true;
    int durationMs = kDurationShortMs;
};

} // namespace dish::models
