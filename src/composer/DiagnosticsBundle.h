// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The diagnostics bundle, `dish-diagnostics.v1` (docs/diagnostics-bundle.md),
// built from the facts the Diagnostics page already shows. Pure and Qt-free:
// the facts come in as plain values, the JSON goes out as text.
//
// What it never holds is a property of the input, not a filter on the
// output: no fact here carries a pairing key, a pairing secret, a certificate
// fingerprint, an IP address or a Wi-Fi name, and hosts have no name field at
// all. A host's label reaches the builder in two places it does not control:
// the flight recorder, whose lines name the host they are about, and the
// snapshot a Satellite sends of its own session. Every label in the log is
// replaced by the host's alias ("Host 1", "Host 2") before anything is written,
// and a label the host list does not know gets the next alias, never itself; a
// host's snapshot has its label and its address replaced by its alias wherever
// they appear, keys included, whatever the host promised about its own
// redaction.

#pragma once

#include "core/diagnostics/DeviceFacts.h"
#include "core/diagnostics/HostSnapshot.h"
#include "core/diagnostics/Json.h"
#include "core/diagnostics/PadLanes.h"
#include "core/diagnostics/PadTestOutcomes.h"
#include "core/diagnostics/WireCounters.h"
#include "core/diagnostics/WriteVerdict.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dish::composer {

struct BundleControllerDb {
    int bundledVersion = 0;
    int activeVersion = 0;
    std::string source;             // "bundled" | "cache"
    std::int64_t lastCheckAtMs = 0; // 0 when never
    std::string lastCheckResult;    // "" when never
};

struct BundleHardware {
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::string product;
    std::string manufacturer;
    // The Direct path's view of the claimed interface: whether sysfs read it at all, and the
    // addresses of its interrupt IN and OUT endpoints where it has them. `endpoints` is null for
    // a pad another path carries.
    bool endpointsRead = false;
    std::optional<std::uint8_t> endpointIn;
    std::optional<std::uint8_t> endpointOut;
};

// The last report a pad published, as it went on the wire.
struct BundleInputState {
    std::uint16_t buttons = 0;
    std::int16_t lx = 0;
    std::int16_t ly = 0;
    std::int16_t rx = 0;
    std::int16_t ry = 0;
    std::uint8_t lt = 0;
    std::uint8_t rt = 0;
    std::uint32_t ext = 0;
};

struct BundleInput {
    std::uint64_t frames = 0;
    int rateHz = 0;
    std::optional<BundleInputState> last;
};

struct BundleLane {
    // "directUsb" | "framework" | "none"
    std::string target = "none";
    diagnostics::LaneSample sample;
};

struct BundleVoice {
    diagnostics::VoiceState state = diagnostics::VoiceState::Closed;
    // The voice plays into the pad's own endpoint, which is the only one this client opens.
    bool onPad = true;
};

struct BundleOutput {
    BundleLane rumble;
    BundleLane lightbar;
    BundleLane playerLeds;
    BundleLane triggerEffects;
    BundleLane micLed;
    // Empty where the pad has no such lane on this PC.
    std::optional<BundleVoice> speaker;
    std::optional<BundleVoice> haptics;
};

// Never an address: the profile and the link layer only.
struct BundleBluetooth {
    std::string profile;        // "hid" | "ble"
    std::string transportLayer; // "classic" | "le"
};

struct BundleController {
    std::optional<std::string> record;
    std::optional<std::string> verification;
    std::string transport; // "usb" | "bluetooth"
    std::string path;      // "direct" | "sdl"
    BundleHardware hardware;
    std::vector<std::string> expectMismatch;
    std::optional<diagnostics::WriteVerdict> lastWrite;
    diagnostics::OutcomeLog outcomes;
    std::optional<BundleInput> input;
    std::optional<BundleOutput> output;
    std::optional<BundleBluetooth> bluetooth;
};

// One controller slot of a session, as this client asked for it and the host applied it. Each
// field is empty where this client does not know it.
struct BundleSessionSlot {
    std::optional<int> index;
    std::optional<std::string> record;
    std::optional<std::string> requestedType;
    std::optional<std::string> appliedType;
    std::optional<std::string> backend;
    std::optional<std::string> result;
    std::optional<std::string> touchpadMode;
};

// The last enriched heartbeat ack's facts.
struct BundleAck {
    bool backendAvailable = false;
    int activeControllers = 0;
    int epoch = 0;
    std::uint16_t activeBitmap = 0;
};

struct BundleRtt {
    double p50Ms = 0.0;
    double p99Ms = 0.0;
    int samples = 0;
};

struct BundleSession {
    std::vector<BundleSessionSlot> controllerSlots;
    std::optional<BundleAck> ack;
    std::optional<BundleRtt> rtt;
    // Empty on a path that counts nothing.
    std::optional<diagnostics::WireCounts> wire;
};

struct BundleCapabilitiesBackend {
    std::string id;
    bool supported = false;
    bool available = false;
    bool audio = false;
};

struct BundleControllerAudio {
    bool enabled = false;
    bool mic = false;
    bool speaker = false;
    bool hapticAudio = false;
};

// What GET /api/server/capabilities last told this client, and the catalog's version.
struct BundleCapabilities {
    std::string serverVersion;
    // The highest protocol the host speaks; every Satellite speaks 1.
    int protocol = 1;
    std::vector<BundleCapabilitiesBackend> backends;
    std::optional<BundleControllerAudio> controllerAudio;
    std::optional<int> catalogVersion;
};

struct BundleMoonlightInfo {
    std::optional<std::string> appVersion;
    std::optional<std::string> state;
    std::optional<int> pairStatus;
};

struct BundleHost {
    std::string kind; // "satellite" | "moonlight"
    // The host's own version, where it said one.
    std::optional<std::string> version;
    int protocol = 0; // 0 before a negotiation settled, and always on a Moonlight host
    std::string link; // the Connections chip token, or the Moonlight phase token
    // False on a Moonlight host, which grants nothing: `grants` is then null.
    bool grantsKnown = true;
    bool mic = false;
    bool speaker = false;
    bool hapticAudio = false;
    std::optional<BundleSession> session;
    // The host's own document, from its diagnostics route.
    std::optional<diagnostics::Json> hostSnapshot;
    // What this client already fetched, written when the host's own document is missing.
    std::optional<BundleCapabilities> capabilities;
    std::optional<BundleMoonlightInfo> moonlight;
    // Why `hostSnapshot` is missing on a Satellite; None on a Moonlight host.
    diagnostics::SnapshotError snapshotError = diagnostics::SnapshotError::None;
};

// "Report a problem" / "Ask a question": the category token, the person's words, the controller.
struct BundleReport {
    std::string category;
    std::string text;
    std::optional<std::string> controller;
};

// One flight recorder line, already in the recorder's tokens. `subjectIsHost` for the link
// events, whose subject is a host's label; `host` is the label a pad was bound to or left.
struct BundleLogEvent {
    std::int64_t atMs = 0;
    std::string kind;
    std::string subject;
    bool subjectIsHost = false;
    std::string host;
    std::vector<std::string> details;
};

struct BundleFacts {
    std::int64_t createdAtMs = 0;
    std::optional<BundleReport> report;
    std::string appVersion;
    // How this copy was installed: `flatpak`, `appimage`, else `github` (a release package or a
    // build of the source).
    std::string flavor = "github";
    std::string model;
    std::string os;
    std::string arch;
    diagnostics::PowerFacts power;
    // Empty where no thermal zone names a trip point.
    std::optional<diagnostics::ThermalState> thermal;
    BundleControllerDb controllerDb;
    std::vector<BundleController> controllers;
    std::vector<BundleHost> hosts;
    // The hosts' labels, in the order of `hosts`: what the log's lines are aliased against.
    std::vector<std::string> hostLabels;
    // The hosts' addresses, in the same order: what a host's own snapshot is scrubbed of.
    std::vector<std::string> hostAddresses;
    std::vector<BundleLogEvent> log;
};

std::string buildDiagnosticsBundle(const BundleFacts& facts);

// "Host 1" for the first host, and so on.
std::string hostAlias(std::size_t index);

} // namespace dish::composer
