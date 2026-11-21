// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/diagnostics/ReportCapture.h"

#include <string>
#include <utility>

namespace dish::diagnostics {

namespace {

Json hexOrNull(const DescriptorBytes& bytes) {
    if (bytes.empty()) { return {}; }
    return {hexOf(bytes)};
}

Json stringsJson(const CaptureStrings& strings) {
    Json out = Json::object();
    out.set("manufacturer", Json::orNull(strings.manufacturer));
    out.set("product", Json::orNull(strings.product));
    return out;
}

Json reportDescriptorsJson(const UsbHardwareFacts& hardware) {
    Json out = Json::array();
    if (!hardware.reportDescriptor.has_value()) { return out; }
    const HidReportDescriptor& descriptor = *hardware.reportDescriptor;
    Json entry = Json::object();
    entry.set("interface", descriptor.interfaceNumber);
    entry.set("hex", hexOf(descriptor.bytes));
    entry.set("source", descriptorSourceToken(descriptor.source));
    out.push(std::move(entry));
    return out;
}

Json restReportsJson(const std::vector<RestReport>& reports) {
    Json out = Json::array();
    for (const RestReport& report : reports) {
        Json entry = Json::object();
        entry.set("atMs", report.atMs);
        entry.set("hex", hexOf(report.bytes));
        out.push(std::move(entry));
    }
    return out;
}

} // namespace

std::vector<RestReport> restReportsOf(const std::vector<CapturedReport>& kept,
                                      std::int64_t loopClockNowMs, std::int64_t wallClockNowMs) {
    std::vector<RestReport> out;
    out.reserve(kept.size());
    for (const CapturedReport& report : kept) {
        const std::int64_t ageMs = loopClockNowMs - report.atMs;
        DescriptorBytes bytes(report.bytes.begin(), report.bytes.begin() + report.length);
        out.push_back(RestReport{wallClockNowMs - ageMs, std::move(bytes)});
    }
    return out;
}

Json captureJson(const UsbHardwareFacts& hardware, const std::vector<RestReport>& reports) {
    Json out = Json::object();
    out.set("format", kCaptureFormat);
    out.set("device", hexOrNull(hardware.raw.device));
    out.set("configuration", hexOrNull(hardware.raw.configuration));
    out.set("strings", stringsJson(hardware.strings));
    out.set("hidReportDescriptors", reportDescriptorsJson(hardware));
    out.set("restReports", restReportsJson(reports));
    return out;
}

} // namespace dish::diagnostics
