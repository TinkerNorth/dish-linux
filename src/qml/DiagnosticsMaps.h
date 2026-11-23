// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Diagnostics page's rows, shaped for QML as tokens and numbers and never
// sentences, so every word the page shows stays in the qsTr catalogues. Split
// out of AppViewModel so the shaping unit-tests without AppModel, which owns
// SDL, USB and timers.

#pragma once

#include "Input/GamepadInputProcessor.h"
#include "composer/CatalogComposer.h"
#include "composer/ConnectionsComposer.h"
#include "composer/DiagnosticsBundle.h"
#include "core/controllerdb/AttachedRecords.h"
#include "core/controllerdb/ControllerCatalog.h"
#include "core/controllerdb/IssueMatch.h"
#include "core/diagnostics/PadTestOutcomes.h"
#include "core/diagnostics/WriteVerdict.h"
#include "core/input/StickHealth.h"
#include "core/reducer/DiagnosticsLog.h"
#include "core/audio/BenchAudio.h"
#include "core/reducer/BenchItems.h"
#include "core/reducer/BenchRuns.h"
#include "core/reducer/HostAudioVerdict.h"
#include "core/reducer/LatencyWindow.h"
#include "core/reducer/ProtocolNegotiation.h"
#include "core/reducer/SessionDiagnostics.h"
#include "source/controllerdb/ControllerDbStore.h"

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <optional>
#include <string>
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

// The name a satellite binding's reasons give its type: the catalog's short
// name for it, as the pad card names it. Empty while the catalog has no row for it.
QString catalogTypeName(const QList<composer::PickableType>& types, int type);

// The touchpad modes as the capability rows take them.
inline constexpr int kTouchpadPickOff = 0;
inline constexpr int kTouchpadPickPad = 1;
inline constexpr int kTouchpadPickMouse = 2;

// The capability rows' touchpad mode for the choice the editors show, named in
// AppSettingsMaps' own words so the two cannot drift. Anything that is not a
// choice, a wire name included, reads as off.
int touchpadPickIndex(const QString& choice);

QVariantMap inputSnapshotRow(const input::GamepadInputProcessor::Inspection& seen);

QVariantMap stickTestRow(const input::StickBench& bench, std::int64_t nowMs);

// The kind a stickTestRow names, read back: empty for a word it never writes.
std::optional<input::StickTestKind> stickTestKindFrom(const QString& token);

// The bench test a page names by its outcome name ("rumble.weak", "lightbar.cycle", ...): empty
// for any name the contract does not list.
std::optional<reducer::BenchTest> benchTestFrom(const QString& name);

// The stick test the inspector's bench runs, by the bench's own name for it.
reducer::BenchTest benchTestOfStickKind(input::StickTestKind kind);

// The items the pad is offered on its path, by their tokens (BenchItems.h's), in the table's
// order: `rumble`, `triggerRumble`, `lightbar`, `playerLeds`, `adaptiveTriggers`, `micLamp`,
// `speaker`, `haptics`, `microphone`, `sticks`.
QStringList benchItemsRow(const reducer::SlotFeedbackInputs& in);

struct BenchStateInputs {
    reducer::BenchRun run;
    audio::MicMeter mic;
    std::optional<reducer::BenchTest> unavailable;
};

// { running, asking, unavailable, micMeter, micPeak }: the first three a test's outcome name or
// "", the meter and the peak whole percents.
QVariantMap benchStateRow(const BenchStateInputs& inputs);

// Oldest first, the order the recorder wrote them.
QVariantList diagnosticsLogRows(const std::vector<reducer::DiagnosticsEvent>& events);

// ── The controller database and the levels of Diagnostics ──────────────────

// { bundledVersion, activeVersion, source, lastCheckAtMs, lastCheckResult, checking }. `source` is
// `bundled` / `cache`; `lastCheckAtMs` is 0 and `lastCheckResult` "" before any check, else a
// result token (`upToDate` / `updated` / `offline` / `invalid` / `off`).
QVariantMap controllerDbRow(const source::controllerdb::ControllerDbStatus& status);

// {} before the pad was written, else { route, reportId, ok, err, atMs }: `route`
// `interruptOut` / `setReport`, `reportId` two hex digits, `err` a winerror.h name or "".
QVariantMap writeVerdictRow(const std::optional<diagnostics::WriteVerdict>& verdict);

// {} before any test, else { test, result, atMs }: `test` one of the bench's outcome names
// (core/reducer/BenchItems.h), `result` `worked` / `didNotWork`.
QVariantMap testOutcomeRow(const std::optional<diagnostics::TestOutcome>& outcome);

struct RecordCardInputs {
    std::optional<controllerdb::AttachedIdentity> identity;
    controllerdb::CatalogPtr catalog;
    std::string locale = "en";
    controllerdb::Transport transport = controllerdb::Transport::Usb;
    controllerdb::HostFacts host;
};

// { matched } and, when matched, { recordId, name, hint, family, chip, inert, mismatches, issues }.
// `chip` is `verified` / `guessed` / `flagged`; `issues` are { id, title, body } for the issues
// that apply to this transport on this machine; the words come from the database's strings in the
// person's language with the English fallback.
QVariantMap controllerRecordRow(const RecordCardInputs& inputs);

// A diagnosticsLog() row as the bundle's log takes it.
composer::BundleLogEvent bundleLogEventOf(const QVariantMap& eventRow);

} // namespace dish::qml
