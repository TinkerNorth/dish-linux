// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Diagnostics page's rows, shaped for QML as tokens and numbers and never
// sentences, so every word the page shows stays in the qsTr catalogues. Split
// out of AppViewModel so the shaping unit-tests without AppModel, which owns
// SDL, USB and timers.

#pragma once

#include "Input/GamepadInputProcessor.h"
#include "composer/ConnectionsComposer.h"
#include "core/input/StickHealth.h"
#include "core/reducer/DiagnosticsLog.h"
#include "core/reducer/HostAudioVerdict.h"
#include "core/reducer/LatencyWindow.h"
#include "core/reducer/ProtocolNegotiation.h"
#include "core/reducer/SessionDiagnostics.h"

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <optional>
#include <vector>

namespace dish::net {
class WifiConnection;
}

namespace dish::qml {

// One satellite session as the host card reads it, taken off the connection and
// its client together on the main thread. The ack-fed fields read -1 until the
// first enriched ack, and stay there while no session is up.
struct HostSessionFacts {
    QString id;
    bool live = false;
    int offeredProtocol = 0;
    int settledProtocol = 0;
    reducer::ProtocolCompat compat = reducer::ProtocolCompat::Unknown;
    int appliedEpoch = -1;
    int hostEpoch = -1;
    int hostBitmap = -1;
    std::uint16_t confirmedBitmap = 0;
    // Kept in the client's own units: small signed counts, -1 before the ack.
    std::int8_t backendAvailable = -1;
    std::int8_t activeControllers = -1;
    int missedAcks = 0;
    bool mouseControlGranted = false;
    reducer::HostAudioVerdict audio;
    reducer::LatencySummary latency;
};

HostSessionFacts hostSessionFactsOf(const net::WifiConnection& conn);

QVariantMap hostDiagnosticsRow(const HostSessionFacts& facts);

// The host card: the session's row, keyed and named by the link as its
// Connections row shows it, so the two pages name one host the same way.
QVariantMap hostCardRow(const composer::ConnectionRow& link, const HostSessionFacts& facts);

// A binding's side of the wire on its satellite: the controller index it holds
// there, what its descriptor declared, and whether the host confirmed it and
// reports it streaming. `declared` is false for a slot the connection carries
// no descriptor for.
struct BindingWireFacts {
    bool declared = false;
    int controllerIndex = 0;
    int type = 0;
    std::uint16_t caps = 0;
    std::uint8_t touchpadMode = 0;
    bool confirmed = false;
    reducer::Streaming streaming = reducer::Streaming::Unknown;
};

BindingWireFacts bindingWireFactsOf(const net::WifiConnection& conn, const QString& slotId);

QVariantMap bindingWireRow(const BindingWireFacts& facts);

// The type a diagnosed binding's capability rows are solved for: the one on
// the wire while a descriptor carries it, else the one the next attach would.
int bindingTypeOf(const BindingWireFacts& wire, int typeForNextAttach);

// The touchpad modes as the capability rows take them.
inline constexpr int kTouchpadPickOff = 0;
inline constexpr int kTouchpadPickPad = 1;
inline constexpr int kTouchpadPickMouse = 2;

// The stored touchpad pick as a capability-row mode. The pick is kept as the
// view model's word ("pad") or the wire's ("ds4"), and both mean the pad routing.
int touchpadPickIndex(const QString& pick);

QVariantMap inputSnapshotRow(const input::GamepadInputProcessor::Inspection& seen);

QVariantMap stickTestRow(const input::StickBench& bench, std::int64_t nowMs);

// The kind a stickTestRow names, read back: empty for a word it never writes.
std::optional<input::StickTestKind> stickTestKindFrom(const QString& token);

// Oldest first, the order the recorder wrote them.
QVariantList diagnosticsLogRows(const std::vector<reducer::DiagnosticsEvent>& events);

} // namespace dish::qml
