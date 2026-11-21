// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What this build can do with a record on the Direct path, and how a served
// record classifies a pad the compiled tables do not name
// (docs/controller-db.md sections 1, 4.2 and 6.6). A record whose `requires`
// names a family or an op this binary lacks is inert: its name and issues
// show, its input and output are never applied, and the bundled row for the
// same id stays in force.

#pragma once

#include "core/controllerdb/ControllerCatalog.h"
#include "core/controllerdb/ControllerRecord.h"
#include "core/controllerdb/Vocabulary.h"
#include "core/input/UsbReportParsers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dish::controllerdb {

using input::usbparse::ButtonOrder;
using input::usbparse::HidParser;

// The Direct parser a family runs on here. The Xbox families are xpad's on Linux (it binds them as
// evdev and publishes no hidraw node), and the N64, Stadia and Bluetooth LE Steam parsers are not
// compiled in.
inline std::optional<HidParser> directParserFor(Family family) {
    switch (family) {
    case Family::DualShock4:
        return HidParser::DualShock4;
    case Family::DualSense:
        return HidParser::DualSense;
    case Family::SwitchProUsb:
        return HidParser::SwitchProUsb;
    case Family::Hid:
        return HidParser::GenericHid;
    case Family::SteamController:
        return HidParser::SteamController;
    case Family::SteamTriton:
        return HidParser::SteamTriton;
    case Family::Xinput360:
    case Family::Xinput360Wireless:
    case Family::XboxOneGip:
    case Family::SwitchN64Usb:
    case Family::Stadia:
    case Family::SteamControllerBle:
    case Family::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

// The button order a hid record names, by the compiled enum it mirrors.
inline std::optional<ButtonOrder> buttonOrderFor(Mapping mapping) {
    switch (mapping) {
    case Mapping::Western:
        return ButtonOrder::Western;
    case Mapping::Switch:
        return ButtonOrder::Switch;
    case Mapping::EightBitDo:
        return ButtonOrder::EightBitDo;
    case Mapping::PlayStationClassic:
        return ButtonOrder::PlayStationClassic;
    case Mapping::None:
    case Mapping::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

// Schema 1 defines no op this build runs, so any op at all is one it lacks.
inline constexpr std::array<std::string_view, 0> kSupportedOps{};

inline bool buildHasFamily(std::string_view token) {
    const Family family = familyFrom(token);
    return directParserFor(family).has_value();
}

inline bool buildHasOp(std::string_view op) {
    return std::find(kSupportedOps.begin(), kSupportedOps.end(), op) != kSupportedOps.end();
}

inline bool isInert(const ControllerRecord& record) {
    const bool lacksAFamily =
        std::any_of(record.requiresFamilies.begin(), record.requiresFamilies.end(),
                    [](const std::string& f) { return !buildHasFamily(f); });
    const bool lacksAnOp = std::any_of(record.requiresOps.begin(), record.requiresOps.end(),
                                       [](const std::string& op) { return !buildHasOp(op); });
    const bool lacksItsParser = !directParserFor(record.family).has_value();
    const bool hidWithoutAnOrder =
        record.family == Family::Hid && !buttonOrderFor(record.mapping).has_value();
    const bool unknownOutput =
        record.route == OutputRoute::Unknown || record.init == InitKind::Unknown;
    return lacksAFamily || lacksAnOp || lacksItsParser || hidWithoutAnOrder || unknownOutput;
}

// The row the Direct path may act on for a pad: the active one, unless it is inert, or flagged
// (failures reported, section 4.1), and the bundled database has a live row for the same id that
// is not, which then stays in force. A flagged row with no such twin still applies; an inert one
// applies nothing.
inline const ControllerRecord* directRecordFor(const ControllerRecord* active,
                                               const ControllerCatalog& bundled) {
    if (active == nullptr) { return nullptr; }
    const bool inert = isInert(*active);
    const bool flagged = active->verification == Verification::Flagged;
    if (!inert && !flagged) { return active; }
    const ControllerRecord* bundledRow = bundled.findById(active->id);
    const bool bundledRowLives = bundledRow != nullptr && !isInert(*bundledRow) &&
                                 bundledRow->verification != Verification::Flagged;
    if (bundledRowLives) { return bundledRow; }
    return inert ? nullptr : active;
}

// Where a Direct pad's parser came from: the tables compiled into this binary, a database row
// for an id those tables lack, or the vendor fallback both leave behind.
enum class ModelSource : std::uint8_t { Compiled, Database, Fallback };

struct DirectModel {
    HidParser parser = HidParser::GenericHid;
    ButtonOrder order = ButtonOrder::Western;
    ModelSource source = ModelSource::Fallback;

    bool operator==(const DirectModel&) const = default;
};

// The ids the compiled tables name: every kKnownModels row and the Sony pads the parser is
// chosen for by product. The Nintendo and other-vendor answers of parserForDevice are a vendor
// guess, not a table, and a record outranks a guess.
inline bool compiledTablesName(int vendorId, int productId) {
    constexpr int kVidSony = 0x054C;
    constexpr std::array<int, 5> kNamedSonyProducts{0x05C4, 0x09CC, 0x0BA0, 0x0CE6,
                                                    input::usbparse::kPidDualSenseEdge};
    if (input::usbparse::lookupKnownModel(vendorId, productId) != nullptr) { return true; }
    const bool namedSony =
        vendorId == kVidSony && std::find(kNamedSonyProducts.begin(), kNamedSonyProducts.end(),
                                          productId) != kNamedSonyProducts.end();
    return namedSony;
}

inline DirectModel compiledModel(int vendorId, int productId, ModelSource source) {
    return DirectModel{input::usbparse::parserForDevice(vendorId, productId),
                       input::usbparse::buttonOrderForDevice(vendorId, productId), source};
}

// Compiled tables first; then a live record (directRecordFor's answer); then the vendor fallback.
inline DirectModel resolveDirectModel(int vendorId, int productId, const ControllerRecord* record) {
    if (compiledTablesName(vendorId, productId)) {
        return compiledModel(vendorId, productId, ModelSource::Compiled);
    }
    const bool recordApplies = record != nullptr && !isInert(*record);
    if (!recordApplies) { return compiledModel(vendorId, productId, ModelSource::Fallback); }
    const HidParser parser = directParserFor(record->family).value_or(HidParser::GenericHid);
    const ButtonOrder order = buttonOrderFor(record->mapping).value_or(ButtonOrder::Western);
    return DirectModel{parser, order, ModelSource::Database};
}

} // namespace dish::controllerdb
