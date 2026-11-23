// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One controller as the fleet's controller database describes it
// (dish-website docs/controller-db.md, schema 1), already parsed: plain values,
// no JSON and no Qt, so every decision over a record is host-testable. A record
// names things this binary already knows how to execute and carries no prose;
// what a person reads comes from the strings table by key.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dish::controllerdb {

inline constexpr int kSchema = 1;

// A USB or Bluetooth identity, "054c:0ce6" on the wire.
struct DeviceId {
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;

    bool operator==(const DeviceId&) const = default;
    std::uint32_t key() const {
        return (static_cast<std::uint32_t>(vendorId) << 16) | static_cast<std::uint32_t>(productId);
    }
};

// input.protocol.ref. Unknown is a family a later database names that this
// build has never heard of: such a record is kept, inert.
enum class Family : std::uint8_t {
    Xinput360,
    Xinput360Wireless,
    XboxOneGip,
    DualShock4,
    DualSense,
    SwitchProUsb,
    SwitchN64Usb,
    Stadia,
    Hid,
    SteamController,
    SteamTriton,
    SteamControllerBle,
    Unknown,
};

// input.mapping, for the hid family only; None where the record carries none.
enum class Mapping : std::uint8_t {
    None,
    Western,
    Switch,
    EightBitDo,
    PlayStationClassic,
    Unknown,
};

enum class DirectRule : std::uint8_t { Auto, OptIn, WhenOsCannot };

// output.route; None is the record's null, a pad with no output at all.
enum class OutputRoute : std::uint8_t { None, InterruptOut, SetReport, Unknown };

enum class InitKind : std::uint8_t {
    None,
    XboxOnePowerOn,
    XboxOneS,
    SwitchProHandshake,
    SteamQuiet,
    SteamTritonQuiet,
    Unknown,
};

enum class Verification : std::uint8_t { Verified, Imported, Crowd, Guessed, Flagged };

enum class Transport : std::uint8_t { Usb, Bluetooth };

// expect.endpoints.out may be null: the maintainer does not know.
enum class Expectation : std::uint8_t { Unknown, Present, Absent };

struct InputCapabilities {
    bool motion = false;
    bool touchpad = false;
    int trackpads = 0;
    bool battery = false;
    bool extendedButtons = false;

    bool operator==(const InputCapabilities&) const = default;
};

struct OutputCapabilities {
    bool rumble = false;
    bool triggerRumble = false;
    bool lightbar = false;
    bool playerLeds = false;
    bool micLed = false;
    bool triggerEffects = false;

    bool operator==(const OutputCapabilities&) const = default;
};

struct Expect {
    Expectation endpointIn = Expectation::Unknown;
    Expectation endpointOut = Expectation::Unknown;

    bool operator==(const Expect&) const = default;
};

// One `os` entry of an issue: {"android": 15} is name "android", version "15".
struct IssueOs {
    std::string name;
    std::string version;

    bool operator==(const IssueOs&) const = default;
};

// support.issues[]: no text, only what decides whether it shows. The title and
// body are strings keys built from the id.
struct Issue {
    std::string id;
    std::optional<std::string> device;
    std::vector<IssueOs> os;
    Transport transport = Transport::Usb;
    std::string lane;
    std::string status;

    bool operator==(const Issue&) const = default;
};

struct ControllerRecord {
    std::string id;
    std::vector<std::string> requiresFamilies;
    std::vector<std::string> requiresOps;
    std::vector<DeviceId> usb;
    std::vector<DeviceId> bluetooth;
    Family family = Family::Unknown;
    Mapping mapping = Mapping::None;
    bool streamsUnprompted = false;
    // platforms.linux.direct.rule where the record has one, else input.direct.rule.
    DirectRule directRule = DirectRule::OptIn;
    InputCapabilities input;
    OutputRoute route = OutputRoute::None;
    InitKind init = InitKind::None;
    OutputCapabilities output;
    Expect expect;
    Verification verification = Verification::Guessed;
    Transport verificationTransport = Transport::Usb;
    std::vector<Issue> issues;
    // False when a section past the identity held a word or a shape this build does not read (a
    // direct rule, a verification state or transport, `requires`, `init.kind`): the row is kept,
    // inert, so the bundled row for its id stays in force.
    bool decoded = true;

    bool operator==(const ControllerRecord&) const = default;
};

struct ControllerDb {
    int schema = kSchema;
    int dbVersion = 0;
    std::vector<ControllerRecord> records;
    // Rows the parser left out because they name no id it can index (no record, no id, a schema it
    // does not read, an identity that does not decode). A row it can index but not read is kept,
    // inert (ControllerRecord::decoded).
    int droppedRows = 0;

    bool operator==(const ControllerDb&) const = default;
};

// strings/<locale>.json: a flat key to text table.
using StringsTable = std::map<std::string, std::string>;

struct ManifestFile {
    std::string path;
    std::string sha256;
    std::int64_t bytes = 0;

    bool operator==(const ManifestFile&) const = default;
};

struct DbManifest {
    int schema = kSchema;
    int dbVersion = 0;
    std::string publishedAt;
    std::vector<ManifestFile> files;

    bool operator==(const DbManifest&) const = default;
};

// The platform this client reads `platforms.<name>` for.
inline constexpr const char* kPlatformName = "linux";

inline DirectRule directRuleFor(std::optional<DirectRule> platformRule, DirectRule general) {
    return platformRule.value_or(general);
}

} // namespace dish::controllerdb
