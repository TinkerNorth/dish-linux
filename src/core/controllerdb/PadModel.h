// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Everything the Direct path and the output builders decide about one pad, resolved once when a
// gateway first enumerates the pad and pinned to its record for as long as it stays
// (docs/controller-db.md sections 6.5 and 6.6). The compiled tables answer first, then the row in
// force for the id (directRecordFor), then the vendor fallback. Every later question about the
// pad (the capability solve, the claim, its read loop, its output worker, the health judgement)
// reads the pinned model, so a database that lands while the pad is attached cannot make them
// disagree, and it applies at the pad's next attach.
//
// A served row supplies what dish-android hands its native table for one (ServedDevice in
// usb_parsers.h): the family's parser, the mapping's button order, the init kind, the Direct rule
// and whether the row is verified. The answers the compiled tables give per product follow from
// the family and the product id, as dish-android's usb_parsers.cpp derives them for a served row
// (modelExpectsFrameworkGamepad, modelSteamTrackpads, modelHasExtendedButtons); a wireless
// receiver's slots are compiled knowledge only, so a served row is never a receiver.

#pragma once

#include "core/controllerdb/ControllerCatalog.h"
#include "core/controllerdb/ControllerRecord.h"
#include "core/controllerdb/DirectModel.h"
#include "core/input/SteamTrackpads.h"
#include "core/input/UsbReportParsers.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dish::controllerdb {

using input::usbparse::ReceiverSlots;
using input::usbparse::SteamTrackpads;

struct PadModel {
    HidParser parser = HidParser::GenericHid;
    ButtonOrder order = ButtonOrder::Western;
    ModelSource source = ModelSource::Fallback;
    InitKind init = InitKind::None;
    // Whether the pad goes Direct without being asked.
    bool autoClaims = false;
    // The compiled row's name; null for a pad no compiled row names.
    const char* compiledName = nullptr;
    // A wireless receiver's slot interfaces, first to last; none when last is below first.
    int receiverFirstSlot = 0;
    int receiverLastSlot = -1;
    bool extendedButtons = false;
    SteamTrackpads steamTrackpads = SteamTrackpads::None;
    // Whether releasing the pad back to Standard produces a framework gamepad: both Steam
    // Controllers stand alone as a keyboard and a mouse.
    bool frameworkGamepadExpected = true;

    bool operator==(const PadModel&) const = default;

    ReceiverSlots receiverSlots() const {
        return ReceiverSlots{receiverFirstSlot, receiverLastSlot};
    }
    bool isReceiver() const { return receiverSlots().any(); }
};

// The vendors whose pads go Direct on their own when no record speaks for them: Sony, Nintendo,
// 8BitDo (both desktops' fast lane).
bool vendorFastLane(int vendorId);

// The init sequence a family's compiled path runs, in the database's words. Every runnable record
// of dbVersion 1 names its family's.
InitKind initOfFamily(HidParser parser);

// The per-product answers, as functions of the family and the product id.
bool isSteamFamily(HidParser parser);
bool extendedButtonsOf(HidParser parser, int productId);
SteamTrackpads steamTrackpadsOf(HidParser parser);

// A database row's Direct rule as a verdict: `auto` on a verified row claims on its own,
// `whenOsCannot` only where the system has no gamepad reading of the pad, `optIn` never.
bool autoClaimsByRule(DirectRule rule, Verification verification, bool frameworkGamepadExpected);

// What an attach pins with no database at all: the compiled tables, then the vendor fallback.
PadModel modelWithoutDatabase(int vendorId, int productId);

// Compiled tables, then the row in force for the id, then the vendor fallback. Either catalog may
// be null; with no bundled catalog an active row applies only while it is live.
PadModel padModelFor(int vendorId, int productId, const ControllerCatalog* active,
                     const ControllerCatalog* bundled);

// The resolver the composition root owns and hands each gateway: an immutable snapshot of the
// active and the bundled catalogs, replaced whole when the database changes. resolve() is safe on
// any thread and takes no lock: it loads the snapshot pointer once. Every snapshot installed stays
// alive until the resolver goes, so a resolve that read the previous one never reads freed
// memory; a database lands once per launch and once per published update, so the resolver keeps a
// handful. install() is called from one thread.
class PadModelResolver {
  public:
    PadModelResolver() = default;
    PadModelResolver(const PadModelResolver&) = delete;
    PadModelResolver& operator=(const PadModelResolver&) = delete;
    PadModelResolver(PadModelResolver&&) = delete;
    PadModelResolver& operator=(PadModelResolver&&) = delete;
    ~PadModelResolver() = default;

    void install(CatalogPtr active, CatalogPtr bundled);
    PadModel resolve(int vendorId, int productId) const;

  private:
    struct Snapshot {
        CatalogPtr active;
        CatalogPtr bundled;
    };

    std::vector<std::unique_ptr<const Snapshot>> kept_;
    std::atomic<const Snapshot*> current_{nullptr};
};

// The models a gateway pinned, one per attach. An attach is named by the platform's own name for
// one enumeration of a device (Linux's HID device name, 0003:054C:0CE6.0007, whose sequence number
// the kernel gives each device it adds), so a pad keeps its model while that name keeps
// enumerating and a pad that comes back is resolved afresh. The gateway's enumeration thread only.
class ModelPins {
  public:
    // The attach's model, resolved now if the attach is new. A null resolver resolves with no
    // database.
    const PadModel& pinFor(const std::string& attach, int vendorId, int productId,
                           const PadModelResolver* resolver);
    // Forgets every attach a scan no longer lists.
    void keepOnly(const std::vector<std::string>& seen);
    std::size_t size() const { return pins_.size(); }

  private:
    std::map<std::string, PadModel> pins_;
};

} // namespace dish::controllerdb
