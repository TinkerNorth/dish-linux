// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/DiagnosticsBundle.h"

#include "core/controllerdb/Vocabulary.h"
#include "core/diagnostics/HostIdentity.h"
#include "core/diagnostics/Json.h"
#include "core/diagnostics/MailHandoff.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace dish::composer {

namespace {

using diagnostics::Json;

constexpr int kBundleVersion = 1;

// Labels to aliases, the host list's order first; a label met only in the log is given the next
// alias the first time it appears.
class HostAliases {
  public:
    explicit HostAliases(std::vector<std::string> labels) : labels_(std::move(labels)) {}

    std::string aliasFor(const std::string& label) {
        const auto it = std::find(labels_.begin(), labels_.end(), label);
        if (it != labels_.end()) {
            return hostAlias(static_cast<std::size_t>(std::distance(labels_.begin(), it)));
        }
        labels_.push_back(label);
        return hostAlias(labels_.size() - 1);
    }

  private:
    std::vector<std::string> labels_;
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

Json deviceJson(const BundleFacts& facts) {
    Json device = Json::object();
    device.set("model", facts.model);
    device.set("os", facts.os);
    device.set("arch", facts.arch);
    return device;
}

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

Json hardwareJson(const BundleHardware& hw) {
    Json out = Json::object();
    out.set("vid", controllerdb::hexWordText(hw.vendorId));
    out.set("pid", controllerdb::hexWordText(hw.productId));
    out.set("product", orNullText(hw.product));
    out.set("manufacturer", orNullText(hw.manufacturer));
    out.set("endpoints", endpointsJson(hw));
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
    out.set("capture", Json());
    return out;
}

Json hostJson(const BundleHost& host, std::size_t index) {
    Json out = Json::object();
    out.set("alias", hostAlias(index));
    out.set("kind", host.kind);
    // The satellite reports no version of its own to this client.
    out.set("version", Json());
    out.set("protocol", host.protocol > 0 ? Json(host.protocol) : Json());
    out.set("link", host.link);
    Json grants = Json::object();
    grants.set("mic", host.mic);
    grants.set("speaker", host.speaker);
    grants.set("hapticAudio", host.hapticAudio);
    out.set("grants", std::move(grants));
    return out;
}

std::string logMessage(const BundleLogEvent& event, HostAliases& aliases) {
    std::string message = event.subjectIsHost ? aliases.aliasFor(event.subject) : event.subject;
    for (const std::string& detail : event.details) {
        if (detail.empty()) { continue; }
        message += " ";
        message += detail;
    }
    if (!event.host.empty()) {
        message += " ";
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
    HostAliases aliases(facts.hostLabels);
    Json bundle = Json::object();
    bundle.set("bundle", kBundleVersion);
    bundle.set("createdAt", diagnostics::isoUtc(facts.createdAtMs));
    bundle.set("app", appJson(facts));
    bundle.set("device", deviceJson(facts));
    bundle.set("controllerDb", controllerDbJson(facts.controllerDb));
    Json controllers = Json::array();
    for (const BundleController& c : facts.controllers) { controllers.push(controllerJson(c)); }
    bundle.set("controllers", std::move(controllers));
    Json hosts = Json::array();
    for (std::size_t i = 0; i < facts.hosts.size(); ++i) {
        hosts.push(hostJson(facts.hosts[i], i));
    }
    bundle.set("hosts", std::move(hosts));
    bundle.set("log", logJson(facts.log, aliases));
    return bundle.dump();
}

} // namespace dish::composer
