// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/DiagnosticsBundle.h"

#include "core/controllerdb/Vocabulary.h"
#include "core/diagnostics/HostIdentity.h"
#include "core/diagnostics/Json.h"
#include "core/diagnostics/MailHandoff.h"
#include "core/model/Protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dish::composer {

namespace {

using diagnostics::Json;

constexpr int kBundleVersion = 1;

// A letter, a digit or a byte of a multi-byte UTF-8 character: what a name must not run into.
bool isWordByte(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) != 0 || byte >= 0x80;
}

bool isDigit(char ch) { return ch >= '0' && ch <= '9'; }

// Whether a name found at [at, end) of `text` stands on its own: where the name's own edge is a
// letter or a digit, the text beside it is not one, nor a dot that continues a number, so
// "10.0.0.1" is not found in "10.0.0.17", "110.0.0.1" or "10.0.0.1.5".
bool standsAlone(std::string_view text, std::size_t at, std::size_t end, std::string_view name) {
    const bool startsWithAWord = isWordByte(name.front());
    const bool endsWithAWord = isWordByte(name.back());
    const bool wordBefore = at > 0 && isWordByte(text[at - 1]);
    const bool numberBefore =
        at > 1 && text[at - 1] == '.' && isDigit(text[at - 2]) && isDigit(name.front());
    const bool wordAfter = end < text.size() && isWordByte(text[end]);
    const bool numberAfter =
        end + 1 < text.size() && text[end] == '.' && isDigit(text[end + 1]) && isDigit(name.back());
    const bool clearBefore = !startsWithAWord || (!wordBefore && !numberBefore);
    const bool clearAfter = !endsWithAWord || (!wordAfter && !numberAfter);
    return clearBefore && clearAfter;
}

// Every listed host's label and address, each the host's one alias, the host list's order first;
// a label met only in the log is given the next alias the first time it appears.
class HostAliases {
  public:
    HostAliases(const std::vector<std::string>& labels, const std::vector<std::string>& addresses)
        : listed_(std::max(labels.size(), addresses.size())) {
        addNames(labels);
        addNames(addresses);
        // Longest first, so a label that holds another host's label is replaced whole.
        std::stable_sort(names_.begin(), names_.end(), [](const Name& a, const Name& b) {
            return a.text.size() > b.text.size();
        });
    }

    std::string aliasFor(const std::string& name) {
        const auto listed = std::find_if(names_.begin(), names_.end(),
                                         [&name](const Name& n) { return n.text == name; });
        if (listed != names_.end()) { return hostAlias(listed->host); }
        const auto seen = std::find(unlisted_.begin(), unlisted_.end(), name);
        const auto offset = static_cast<std::size_t>(std::distance(unlisted_.begin(), seen));
        if (seen == unlisted_.end()) { unlisted_.push_back(name); }
        return hostAlias(listed_ + offset);
    }

    // One pass, left to right: each listed name that stands alone becomes its host's alias, and
    // nothing written in its place is read again.
    std::string scrubbed(std::string_view text) const {
        std::string out;
        std::size_t at = 0;
        while (at < text.size()) {
            const Name* found = nameAt(text, at);
            if (found == nullptr) {
                out.push_back(text[at]);
                ++at;
                continue;
            }
            out += hostAlias(found->host);
            at += found->text.size();
        }
        return out;
    }

  private:
    struct Name {
        std::string text;
        std::size_t host = 0;
    };

    void addNames(const std::vector<std::string>& texts) {
        for (std::size_t host = 0; host < texts.size(); ++host) {
            if (!texts[host].empty()) { names_.push_back(Name{texts[host], host}); }
        }
    }

    const Name* nameAt(std::string_view text, std::size_t at) const {
        for (const Name& name : names_) {
            const std::size_t end = at + name.text.size();
            const bool here = text.compare(at, name.text.size(), name.text) == 0;
            if (here && standsAlone(text, at, end, name.text)) { return &name; }
        }
        return nullptr;
    }

    std::size_t listed_ = 0;
    std::vector<Name> names_;
    std::vector<std::string> unlisted_;
};

Json orNullText(const std::string& text) { return text.empty() ? Json() : Json(text); }

Json appJson(const BundleFacts& facts) {
    Json app = Json::object();
    app.set("name", diagnostics::kAppName);
    app.set("version", facts.appVersion);
    app.set("build", diagnostics::buildCodeOf(facts.appVersion));
    app.set("flavor", facts.flavor);
    return app;
}

Json batteryJson(const std::optional<diagnostics::BatteryFacts>& battery) {
    if (!battery.has_value()) { return {}; }
    Json out = Json::object();
    out.set("level", Json::orNull(battery->level));
    out.set("charging", battery->charging);
    return out;
}

// `thermal` from the thermal zones' trip points where a zone names one; `wifi` is null because
// this client links no WLAN library (tests/PARITY.md); `usbHost` as the reader measured it.
Json deviceJson(const BundleFacts& facts) {
    Json device = Json::object();
    device.set("model", facts.model);
    device.set("os", facts.os);
    device.set("arch", facts.arch);
    device.set("battery", batteryJson(facts.power.battery));
    device.set("thermal", facts.thermal.has_value()
                              ? Json(diagnostics::thermalStateToken(*facts.thermal))
                              : Json());
    device.set("powerSave", Json::orNull(facts.power.powerSave));
    device.set("wifi", Json());
    device.set("usbHost", Json::orNull(facts.usbHost));
    return device;
}

Json reportJson(const BundleReport& report) {
    Json out = Json::object();
    out.set("category", report.category);
    out.set("text", report.text);
    out.set("controller", Json::orNull(report.controller));
    return out;
}

// Lowercase hex, zero-padded to `digits`.
std::string hexText(std::uint64_t value, int digits) {
    constexpr std::string_view kHex = "0123456789abcdef";
    constexpr unsigned kNibbleBits = 4;
    constexpr std::uint64_t kNibbleMask = 0xF;
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[static_cast<std::size_t>(value & kNibbleMask)];
        value >>= kNibbleBits;
    }
    return out;
}

constexpr int kWordDigits = 4;
constexpr int kLongDigits = 8;
constexpr int kByteDigits = 2;

Json count(std::uint64_t value) { return {static_cast<std::int64_t>(value)}; }

Json controllerDbJson(const BundleControllerDb& db) {
    Json out = Json::object();
    out.set("bundledVersion", db.bundledVersion);
    out.set("activeVersion", db.activeVersion);
    out.set("source", db.source);
    out.set("lastCheckAt",
            db.lastCheckAtMs > 0 ? Json(diagnostics::isoUtc(db.lastCheckAtMs)) : Json());
    out.set("lastCheckResult", orNullText(db.lastCheckResult));
    return out;
}

Json endpointJson(const std::optional<std::uint8_t>& address) {
    if (!address.has_value()) { return {}; }
    return {diagnostics::hexByteText(*address)};
}

Json endpointsJson(const BundleHardware& hw) {
    if (!hw.endpointsRead) { return {}; }
    Json out = Json::object();
    out.set("in", endpointJson(hw.endpointIn));
    out.set("out", endpointJson(hw.endpointOut));
    return out;
}

Json bcdJson(const std::optional<std::uint16_t>& bcd) {
    if (!bcd.has_value()) { return {}; }
    return {diagnostics::bcdText(*bcd)};
}

Json interfacesOrNull(const std::optional<std::vector<diagnostics::UsbInterface>>& interfaces) {
    if (!interfaces.has_value()) { return {}; }
    return diagnostics::interfacesJson(*interfaces);
}

// `firmware` is null: no desktop path reads a pad's firmware version.
Json hardwareJson(const BundleHardware& hw) {
    Json out = Json::object();
    out.set("vid", controllerdb::hexWordText(hw.vendorId));
    out.set("pid", controllerdb::hexWordText(hw.productId));
    out.set("bcdDevice", bcdJson(hw.bcdDevice));
    out.set("product", orNullText(hw.product));
    out.set("manufacturer", orNullText(hw.manufacturer));
    out.set("interfaces", interfacesOrNull(hw.interfaces));
    out.set("endpoints", endpointsJson(hw));
    out.set("descriptorSha256", Json::orNull(hw.descriptorSha256));
    out.set("firmware", Json());
    return out;
}

Json lastWriteJson(const std::optional<diagnostics::WriteVerdict>& verdict) {
    if (!verdict.has_value()) { return {}; }
    Json out = Json::object();
    out.set("route", diagnostics::writeRouteToken(verdict->route));
    out.set("reportId", diagnostics::reportIdText(verdict->reportId));
    out.set("ok", verdict->ok);
    out.set("err", orNullText(diagnostics::osErrorName(verdict->osError)));
    out.set("atMs", verdict->atMs);
    return out;
}

Json outcomesJson(const diagnostics::OutcomeLog& outcomes) {
    Json out = Json::array();
    for (const diagnostics::TestOutcome& outcome : outcomes) {
        Json entry = Json::object();
        entry.set("test", outcome.test);
        entry.set("result", outcome.result);
        entry.set("atMs", outcome.atMs);
        out.push(std::move(entry));
    }
    return out;
}

Json textArray(const std::vector<std::string>& texts) {
    Json out = Json::array();
    for (const std::string& text : texts) { out.push(text); }
    return out;
}

Json inputStateJson(const BundleInputState& state) {
    Json out = Json::object();
    out.set("buttons", hexText(state.buttons, kWordDigits));
    out.set("lx", static_cast<int>(state.lx));
    out.set("ly", static_cast<int>(state.ly));
    out.set("rx", static_cast<int>(state.rx));
    out.set("ry", static_cast<int>(state.ry));
    out.set("lt", static_cast<int>(state.lt));
    out.set("rt", static_cast<int>(state.rt));
    out.set("ext", hexText(state.ext, kLongDigits));
    return out;
}

Json inputJson(const std::optional<BundleInput>& input) {
    if (!input.has_value()) { return {}; }
    Json out = Json::object();
    out.set("frames", count(input->frames));
    out.set("rateHz", input->rateHz);
    out.set("last", input->last.has_value() ? inputStateJson(*input->last) : Json());
    return out;
}

// The lane's last value in its own words; null before the lane carried anything.
using LastValue = Json (*)(std::uint64_t packed);

Json rumbleLast(std::uint64_t packed) {
    Json out = Json::object();
    out.set("strong", static_cast<int>(diagnostics::rumbleStrongOf(packed)));
    out.set("weak", static_cast<int>(diagnostics::rumbleWeakOf(packed)));
    return out;
}

Json rgbLast(std::uint64_t packed) {
    Json out = Json::object();
    out.set("r", static_cast<int>(diagnostics::rgbRedOf(packed)));
    out.set("g", static_cast<int>(diagnostics::rgbGreenOf(packed)));
    out.set("b", static_cast<int>(diagnostics::rgbBlueOf(packed)));
    return out;
}

Json maskLast(std::uint64_t packed) {
    Json out = Json::object();
    out.set("mask", hexText(packed, kByteDigits));
    return out;
}

Json stateLast(std::uint64_t packed) {
    Json out = Json::object();
    out.set("state", static_cast<std::int64_t>(packed));
    return out;
}

// The trigger effects' 22 bytes are the host's own, kept by no lane: only when.
Json whenOnlyLast(std::uint64_t /*packed*/) { return Json::object(); }

Json laneJson(const BundleLane& lane, LastValue lastValue) {
    Json out = Json::object();
    out.set("count", count(lane.sample.count));
    out.set("target", lane.target);
    Json last;
    if (lane.sample.lastAtMs > 0) {
        last = lastValue(lane.sample.last);
        last.set("atMs", lane.sample.lastAtMs);
    }
    out.set("last", std::move(last));
    return out;
}

// The voice plays only into the pad's own endpoint; per-voice frames are not counted here.
Json voiceJson(const std::optional<BundleVoice>& voice) {
    if (!voice.has_value()) { return {}; }
    const bool open = voice->state == diagnostics::VoiceState::Open;
    Json out = Json::object();
    out.set("voice", diagnostics::voiceStateToken(voice->state));
    out.set("device", open && voice->onPad ? Json("pad") : Json());
    out.set("frames", Json());
    return out;
}

// No desktop path delivers a trigger-motor rumble, so that lane is null.
Json outputJson(const BundleController& c) {
    if (!c.output.has_value()) { return {}; }
    const BundleOutput& output = *c.output;
    Json rumble = laneJson(output.rumble, &rumbleLast);
    rumble.set("lastWrite", lastWriteJson(c.lastWrite));
    Json out = Json::object();
    out.set("rumble", std::move(rumble));
    out.set("triggerRumble", Json());
    out.set("lightbar", laneJson(output.lightbar, &rgbLast));
    out.set("playerLeds", laneJson(output.playerLeds, &maskLast));
    out.set("triggerEffects", laneJson(output.triggerEffects, &whenOnlyLast));
    out.set("micLed", laneJson(output.micLed, &stateLast));
    out.set("speaker", voiceJson(output.speaker));
    out.set("haptics", voiceJson(output.haptics));
    return out;
}

Json bluetoothJson(const std::optional<BundleBluetooth>& bluetooth) {
    if (!bluetooth.has_value()) { return {}; }
    Json out = Json::object();
    out.set("profile", bluetooth->profile);
    out.set("transportLayer", orNullText(bluetooth->transportLayer));
    return out;
}

Json controllerJson(const BundleController& c) {
    Json out = Json::object();
    out.set("record", Json::orNull(c.record));
    out.set("verification", Json::orNull(c.verification));
    out.set("transport", c.transport);
    out.set("path", c.path);
    out.set("hardware", hardwareJson(c.hardware));
    out.set("expectMismatch", textArray(c.expectMismatch));
    // No desktop source records feedback arrivals per pad yet (tests/PARITY.md).
    out.set("lastFeedback", Json());
    out.set("lastWrite", lastWriteJson(c.lastWrite));
    out.set("outcomes", outcomesJson(c.outcomes));
    out.set("capture", c.capture.has_value() ? *c.capture : Json());
    out.set("input", inputJson(c.input));
    out.set("output", outputJson(c));
    out.set("bluetooth", bluetoothJson(c.bluetooth));
    return out;
}

Json optionalText(const std::optional<std::string>& text) { return Json::orNull(text); }

Json sessionSlotJson(const BundleSessionSlot& slot) {
    Json out = Json::object();
    out.set("index", Json::orNull(slot.index));
    out.set("record", optionalText(slot.record));
    out.set("requestedType", optionalText(slot.requestedType));
    out.set("appliedType", optionalText(slot.appliedType));
    out.set("backend", optionalText(slot.backend));
    out.set("result", optionalText(slot.result));
    out.set("touchpadMode", optionalText(slot.touchpadMode));
    return out;
}

Json ackJson(const std::optional<BundleAck>& ack) {
    if (!ack.has_value()) { return {}; }
    Json out = Json::object();
    out.set("backendAvailable", ack->backendAvailable);
    out.set("activeControllers", ack->activeControllers);
    out.set("epoch", ack->epoch);
    out.set("activeBitmap", hexText(ack->activeBitmap, kWordDigits));
    return out;
}

Json rttJson(const std::optional<BundleRtt>& rtt) {
    if (!rtt.has_value()) { return {}; }
    Json out = Json::object();
    out.set("p50Ms", rtt->p50Ms);
    out.set("p99Ms", rtt->p99Ms);
    out.set("samples", rtt->samples);
    return out;
}

template <typename Kind, std::size_t N>
Json wireCountsJson(const std::array<diagnostics::WireKindRow<Kind>, N>& rows,
                    const std::array<std::uint64_t, N>& counts) {
    Json out = Json::object();
    for (std::size_t i = 0; i < N; ++i) { out.set(rows[i].token, count(counts[i])); }
    return out;
}

Json sessionJson(const std::optional<BundleSession>& session) {
    if (!session.has_value()) { return {}; }
    Json rows = Json::array();
    for (const BundleSessionSlot& slot : session->controllerSlots) {
        rows.push(sessionSlotJson(slot));
    }
    Json out = Json::object();
    out.set("slots", std::move(rows));
    out.set("ack", ackJson(session->ack));
    out.set("rtt", rttJson(session->rtt));
    if (session->wire.has_value()) {
        out.set("sent", wireCountsJson(diagnostics::kSentKinds, session->wire->sent));
        out.set("received", wireCountsJson(diagnostics::kReceivedKinds, session->wire->received));
    } else {
        out.set("sent", Json());
        out.set("received", Json());
    }
    return out;
}

Json capabilitiesJson(const BundleCapabilities& caps) {
    Json protocol = Json::array();
    protocol.push(proto::kProtocolVersionMin);
    protocol.push(caps.protocol);
    Json backends = Json::array();
    for (const BundleCapabilitiesBackend& backend : caps.backends) {
        Json entry = Json::object();
        entry.set("id", backend.id);
        entry.set("supported", backend.supported);
        entry.set("available", backend.available);
        entry.set("audio", backend.audio);
        backends.push(std::move(entry));
    }
    Json audio;
    if (caps.controllerAudio.has_value()) {
        audio = Json::object();
        audio.set("enabled", caps.controllerAudio->enabled);
        audio.set("mic", caps.controllerAudio->mic);
        audio.set("speaker", caps.controllerAudio->speaker);
        audio.set("hapticAudio", caps.controllerAudio->hapticAudio);
    }
    Json out = Json::object();
    out.set("format", diagnostics::kCapabilitiesSnapshotFormat);
    out.set("serverVersion", orNullText(caps.serverVersion));
    out.set("protocol", std::move(protocol));
    out.set("backends", std::move(backends));
    out.set("controllerAudio", std::move(audio));
    out.set("catalogVersion", Json::orNull(caps.catalogVersion));
    return out;
}

Json moonlightJson(const BundleMoonlightInfo& info) {
    Json out = Json::object();
    out.set("format", diagnostics::kMoonlightSnapshotFormat);
    out.set("appVersion", optionalText(info.appVersion));
    out.set("state", optionalText(info.state));
    out.set("pairStatus", Json::orNull(info.pairStatus));
    return out;
}

// Every string and every key, at every depth, of every listed host's names.
Json scrubbed(const Json& value, const HostAliases& aliases) {
    switch (value.kind()) {
    case Json::Kind::String:
        return {aliases.scrubbed(value.stringValue())};
    case Json::Kind::Array: {
        Json out = Json::array();
        for (const Json& item : value.items()) { out.push(scrubbed(item, aliases)); }
        return out;
    }
    case Json::Kind::Object: {
        Json out = Json::object();
        for (std::size_t i = 0; i < value.keys().size(); ++i) {
            out.set(aliases.scrubbed(value.keys()[i]), scrubbed(value.items()[i], aliases));
        }
        return out;
    }
    case Json::Kind::Null:
    case Json::Kind::Bool:
    case Json::Kind::Integer:
    case Json::Kind::Double:
        return value;
    }
    return value;
}

// The host's own document when it sent one, else what this client already knew about it.
Json snapshotJson(const BundleHost& host, const HostAliases& aliases) {
    if (host.moonlight.has_value()) { return moonlightJson(*host.moonlight); }
    if (host.hostSnapshot.has_value()) { return scrubbed(*host.hostSnapshot, aliases); }
    if (host.capabilities.has_value()) { return capabilitiesJson(*host.capabilities); }
    return {};
}

Json grantsJson(const BundleHost& host) {
    if (!host.grantsKnown) { return {}; }
    Json grants = Json::object();
    grants.set("mic", host.mic);
    grants.set("speaker", host.speaker);
    grants.set("hapticAudio", host.hapticAudio);
    return grants;
}

// The version the host itself reported; an older Satellite's comes from its capabilities.
Json hostVersionJson(const BundleHost& host) {
    if (host.version.has_value()) { return {*host.version}; }
    if (host.capabilities.has_value()) { return orNullText(host.capabilities->serverVersion); }
    return {};
}

Json hostJson(const BundleHost& host, std::size_t index, const HostAliases& aliases) {
    const std::optional<std::string_view> error =
        diagnostics::snapshotErrorToken(host.snapshotError);
    Json out = Json::object();
    out.set("alias", hostAlias(index));
    out.set("kind", host.kind);
    out.set("version", hostVersionJson(host));
    out.set("protocol", host.protocol > 0 ? Json(host.protocol) : Json());
    out.set("link", host.link);
    out.set("grants", grantsJson(host));
    out.set("session", sessionJson(host.session));
    out.set("snapshot", snapshotJson(host, aliases));
    out.set("snapshotError", error.has_value() ? Json(*error) : Json());
    return out;
}

// The pieces are scrubbed before they are joined, so an alias already written is never read
// as a name.
std::string logMessage(const BundleLogEvent& event, HostAliases& aliases) {
    std::string message =
        event.subjectIsHost ? aliases.aliasFor(event.subject) : aliases.scrubbed(event.subject);
    for (const std::string& detail : event.details) {
        if (detail.empty()) { continue; }
        message += ' ';
        message += aliases.scrubbed(detail);
    }
    if (!event.host.empty()) {
        message += ' ';
        message += aliases.aliasFor(event.host);
    }
    return message;
}

Json logJson(const std::vector<BundleLogEvent>& log, HostAliases& aliases) {
    Json out = Json::array();
    for (const BundleLogEvent& event : log) {
        Json line = Json::object();
        line.set("atMs", event.atMs);
        line.set("tag", event.kind);
        line.set("message", logMessage(event, aliases));
        out.push(std::move(line));
    }
    return out;
}

} // namespace

std::string hostAlias(std::size_t index) { return "Host " + std::to_string(index + 1); }

std::string buildDiagnosticsBundle(const BundleFacts& facts) {
    HostAliases aliases(facts.hostLabels, facts.hostAddresses);
    Json bundle = Json::object();
    bundle.set("bundle", kBundleVersion);
    bundle.set("createdAt", diagnostics::isoUtc(facts.createdAtMs));
    if (facts.report.has_value()) { bundle.set("report", reportJson(*facts.report)); }
    bundle.set("app", appJson(facts));
    bundle.set("device", deviceJson(facts));
    bundle.set("controllerDb", controllerDbJson(facts.controllerDb));
    Json controllers = Json::array();
    for (const BundleController& c : facts.controllers) { controllers.push(controllerJson(c)); }
    bundle.set("controllers", std::move(controllers));
    Json hosts = Json::array();
    for (std::size_t i = 0; i < facts.hosts.size(); ++i) {
        hosts.push(hostJson(facts.hosts[i], i, aliases));
    }
    bundle.set("hosts", std::move(hosts));
    bundle.set("log", logJson(facts.log, aliases));
    return bundle.dump();
}

} // namespace dish::composer
