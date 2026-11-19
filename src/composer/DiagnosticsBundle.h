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
// all. The one place a host's label reaches the builder is the flight
// recorder, whose lines name the host they are about; every such label is
// replaced by the host's alias ("Host 1", "Host 2") before anything is written,
// and a label the host list does not know gets the next alias, never itself.

#pragma once

#include "core/diagnostics/PadTestOutcomes.h"
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

struct BundleController {
    std::optional<std::string> record;
    std::optional<std::string> verification;
    std::string transport; // "usb" | "bluetooth"
    std::string path;      // "direct" | "sdl"
    BundleHardware hardware;
    std::vector<std::string> expectMismatch;
    std::optional<diagnostics::WriteVerdict> lastWrite;
    diagnostics::OutcomeLog outcomes;
};

struct BundleHost {
    std::string kind; // "satellite"
    int protocol = 0; // 0 before a negotiation settled
    std::string link; // the Connections chip token
    bool mic = false;
    bool speaker = false;
    bool hapticAudio = false;
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
    std::string appVersion;
    // How this copy was installed: `flatpak`, `appimage`, else `github` (a release package or a
    // build of the source).
    std::string flavor = "github";
    std::string model;
    std::string os;
    std::string arch;
    BundleControllerDb controllerDb;
    std::vector<BundleController> controllers;
    std::vector<BundleHost> hosts;
    // The hosts' labels, in the order of `hosts`: what the log's lines are aliased against.
    std::vector<std::string> hostLabels;
    std::vector<BundleLogEvent> log;
};

std::string buildDiagnosticsBundle(const BundleFacts& facts);

// "Host 1" for the first host, and so on.
std::string hostAlias(std::size_t index);

} // namespace dish::composer
