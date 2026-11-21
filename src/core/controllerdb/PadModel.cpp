// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/controllerdb/PadModel.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace dish::controllerdb {

namespace {

constexpr int kVidSony = 0x054C;
constexpr int kVidNintendo = 0x057E;
constexpr int kVid8BitDo = 0x2DC8;
constexpr int kPidSwitchPro = 0x2009;
constexpr int kPidSwitchChargingGrip = 0x200E;

// The answers that follow from the family and the product id, for a model whatever its source.
void deriveFromFamily(PadModel& model, int productId) {
    model.extendedButtons = extendedButtonsOf(model.parser, productId);
    model.steamTrackpads = steamTrackpadsOf(model.parser);
    model.frameworkGamepadExpected = !isSteamFamily(model.parser);
}

PadModel compiledPadModel(int vendorId, int productId, ModelSource source) {
    const DirectModel direct = compiledModel(vendorId, productId, source);
    const input::usbparse::KnownHidModel* known =
        input::usbparse::lookupKnownModel(vendorId, productId);
    PadModel model;
    model.parser = direct.parser;
    model.order = direct.order;
    model.source = direct.source;
    model.init = initOfFamily(direct.parser);
    model.autoClaims = vendorFastLane(vendorId);
    model.compiledName = known != nullptr ? known->name : nullptr;
    const ReceiverSlots slots = input::usbparse::receiverSlotsFor(vendorId, productId);
    model.receiverFirstSlot = slots.first;
    model.receiverLastSlot = slots.last;
    deriveFromFamily(model, productId);
    return model;
}

PadModel databasePadModel(int vendorId, int productId, const ControllerRecord& row) {
    const DirectModel direct = resolveDirectModel(vendorId, productId, &row);
    PadModel model;
    model.parser = direct.parser;
    model.order = direct.order;
    model.source = direct.source;
    model.init = row.init;
    deriveFromFamily(model, productId);
    model.autoClaims =
        autoClaimsByRule(row.directRule, row.verification, model.frameworkGamepadExpected);
    return model;
}

// The row a pad's id names in the active catalog, with the bundled one standing in for an inert
// or a flagged row (directRecordFor); with no bundled catalog, the active row while it is live.
const ControllerRecord* rowInForce(DeviceId id, const ControllerCatalog* active,
                                   const ControllerCatalog* bundled) {
    const ControllerRecord* activeRow = active != nullptr ? active->findUsb(id) : nullptr;
    if (bundled != nullptr) { return directRecordFor(activeRow, *bundled); }
    const bool live = activeRow != nullptr && !isInert(*activeRow);
    return live ? activeRow : nullptr;
}

} // namespace

bool vendorFastLane(int vendorId) {
    return vendorId == kVidSony || vendorId == kVidNintendo || vendorId == kVid8BitDo;
}

InitKind initOfFamily(HidParser parser) {
    switch (parser) {
    case HidParser::SteamController:
        return InitKind::SteamQuiet;
    case HidParser::SteamTriton:
        return InitKind::SteamTritonQuiet;
    case HidParser::SwitchProUsb:
        return InitKind::SwitchProHandshake;
    case HidParser::DualShock4:
    case HidParser::DualSense:
    case HidParser::GenericHid:
    case HidParser::None:
        return InitKind::None;
    }
    return InitKind::None;
}

bool isSteamFamily(HidParser parser) {
    return parser == HidParser::SteamController || parser == HidParser::SteamTriton;
}

// The DualSense decoder reads the Edge's back and Fn buttons only for the Edge's product id; the
// Switch Pro decoder reads Capture for any id, and the Pro and the charging grip have one.
bool extendedButtonsOf(HidParser parser, int productId) {
    const bool isTheEdge =
        parser == HidParser::DualSense && productId == input::usbparse::kPidDualSenseEdge;
    const bool hasCapture = parser == HidParser::SwitchProUsb &&
                            (productId == kPidSwitchPro || productId == kPidSwitchChargingGrip);
    return isSteamFamily(parser) || isTheEdge || hasCapture;
}

SteamTrackpads steamTrackpadsOf(HidParser parser) {
    switch (parser) {
    case HidParser::SteamController:
        return SteamTrackpads::Pad2015;
    case HidParser::SteamTriton:
        return SteamTrackpads::Pad2026;
    case HidParser::DualShock4:
    case HidParser::DualSense:
    case HidParser::SwitchProUsb:
    case HidParser::GenericHid:
    case HidParser::None:
        return SteamTrackpads::None;
    }
    return SteamTrackpads::None;
}

bool autoClaimsByRule(DirectRule rule, Verification verification, bool frameworkGamepadExpected) {
    const bool verified = verification == Verification::Verified;
    switch (rule) {
    case DirectRule::Auto:
        return verified;
    case DirectRule::WhenOsCannot:
        return verified && !frameworkGamepadExpected;
    case DirectRule::OptIn:
        return false;
    }
    return false;
}

PadModel modelWithoutDatabase(int vendorId, int productId) {
    return padModelFor(vendorId, productId, nullptr, nullptr);
}

PadModel padModelFor(int vendorId, int productId, const ControllerCatalog* active,
                     const ControllerCatalog* bundled) {
    if (compiledTablesName(vendorId, productId)) {
        return compiledPadModel(vendorId, productId, ModelSource::Compiled);
    }
    const DeviceId id{static_cast<std::uint16_t>(vendorId), static_cast<std::uint16_t>(productId)};
    const ControllerRecord* row = rowInForce(id, active, bundled);
    if (row == nullptr) { return compiledPadModel(vendorId, productId, ModelSource::Fallback); }
    return databasePadModel(vendorId, productId, *row);
}

void PadModelResolver::install(CatalogPtr active, CatalogPtr bundled) {
    auto snapshot =
        std::make_unique<const Snapshot>(Snapshot{std::move(active), std::move(bundled)});
    const Snapshot* published = snapshot.get();
    kept_.push_back(std::move(snapshot));
    current_.store(published, std::memory_order_release);
}

PadModel PadModelResolver::resolve(int vendorId, int productId) const {
    const Snapshot* snapshot = current_.load(std::memory_order_acquire);
    if (snapshot == nullptr) { return modelWithoutDatabase(vendorId, productId); }
    return padModelFor(vendorId, productId, snapshot->active.get(), snapshot->bundled.get());
}

const PadModel& ModelPins::pinFor(const std::string& attach, int vendorId, int productId,
                                  const PadModelResolver* resolver) {
    const auto pinned = pins_.find(attach);
    if (pinned != pins_.end()) { return pinned->second; }
    const PadModel model = resolver != nullptr ? resolver->resolve(vendorId, productId)
                                               : modelWithoutDatabase(vendorId, productId);
    return pins_.emplace(attach, model).first->second;
}

void ModelPins::keepOnly(const std::vector<std::string>& seen) {
    std::erase_if(pins_, [&seen](const auto& entry) {
        return std::find(seen.begin(), seen.end(), entry.first) == seen.end();
    });
}

} // namespace dish::controllerdb
