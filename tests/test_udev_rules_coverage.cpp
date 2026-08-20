// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// packaging/udev/70-dish-hidraw.rules is what makes USB-direct reachable at
// all: /dev/hidraw* is 0600 root:root, so a model the code will open but the
// rules never name fails every claim with PermissionDenied and drops silently
// back to the rate-capped SDL path. Nothing in the build ties the two together
// — a new row in kKnownModels or a new fast-lane vendor compiles, links, ships,
// and is broken on every machine. This is that tie.
//
// The claimable set is derived from the code, not restated here: the table is
// walked directly and the vendor-wide lanes are found by scanning the id space
// through parserForDevice + isKnownFastLaneModel, so a new lane fails this test
// rather than passing a list someone forgot to update.
//
// KNOWN GAP, deliberately not gated: parserForDevice returns GenericHid for any
// unlisted vid:pid, and enumerate() admits it on the strength of its collection
// alone. A Logitech, Hori or Thrustmaster pad therefore enumerates, offers a
// Direct pick, and fails PermissionDenied — no rule covers it and none could
// short of a catch-all that would loosen every HID device on the system. Only
// the ENUMERATED models below are gated; the generic lane's claim failure is
// the designed outcome, not a regression this test can catch.

#include "core/input/UsbReportParsers.h"
#include "source/usb/HidrawGateway.h"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using dish::input::usbparse::HidParser;
using dish::input::usbparse::kKnownModels;
using dish::input::usbparse::parserForDevice;
using dish::source::usb::HidrawGateway;

namespace {

// One `ATTRS{idVendor}==` line of the rules file. No product means the line
// matches every product of that vendor.
struct Rule {
    std::string vendor;
    std::string product; // empty = vendor-wide
    bool grantsAccess = false;
    int line = 0;
};

std::string lowerHex(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

// A vid/pid the way sysfs publishes it: four lowercase hex digits.
std::string idText(int id) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (int shift = 12; shift >= 0; shift -= 4) { out.push_back(kDigits[(id >> shift) & 0xF]); }
    return out;
}

std::string rulesText() {
    std::ifstream in(DISH_UDEV_RULES_PATH);
    REQUIRE(in.good());
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// The value of `key=="<value>"` on one line, or of `key="<value>"` for an
// assignment. Literal only: a glob in a rule would not be recognised as
// coverage, which fails loudly rather than passing quietly.
std::optional<std::string> quotedValue(const std::string& line, const std::string& key) {
    const std::size_t at = line.find(key);
    if (at == std::string::npos) { return std::nullopt; }
    const std::size_t open = line.find('"', at + key.size());
    if (open == std::string::npos) { return std::nullopt; }
    const std::size_t close = line.find('"', open + 1);
    if (close == std::string::npos) { return std::nullopt; }
    return line.substr(open + 1, close - open - 1);
}

std::vector<Rule> readRules() {
    std::istringstream in(rulesText());
    std::vector<Rule> rules;
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        number++;
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') { continue; }
        const auto vendor = quotedValue(line, "ATTRS{idVendor}==");
        if (!vendor) { continue; }
        Rule rule;
        rule.vendor = lowerHex(*vendor);
        if (const auto product = quotedValue(line, "ATTRS{idProduct}==")) {
            rule.product = lowerHex(*product);
        }
        // A line that matches but hands out nothing leaves the node root-only,
        // so coverage has to mean granted, not merely mentioned.
        rule.grantsAccess =
            line.find("MODE=") != std::string::npos || line.find("uaccess") != std::string::npos;
        rule.line = number;
        rules.push_back(rule);
    }
    return rules;
}

// A vendor-wide line covers every product of the vendor, which is what the
// fast lane and the Sony/Nintendo parser lanes need.
bool coversWholeVendor(const std::vector<Rule>& rules, int vid) {
    for (const Rule& rule : rules) {
        if (rule.grantsAccess && rule.product.empty() && rule.vendor == idText(vid)) {
            return true;
        }
    }
    return false;
}

bool covers(const std::vector<Rule>& rules, int vid, int pid) {
    for (const Rule& rule : rules) {
        if (!rule.grantsAccess || rule.vendor != idText(vid)) { continue; }
        if (rule.product.empty() || rule.product == idText(pid)) { return true; }
    }
    return false;
}

// Stand-ins for "some product this vendor ships tomorrow": they exercise the
// lanes that key on the vendor alone, so only a vendor-wide rule can cover one.
constexpr int kUnlistedPids[] = {0x0000, 0x0001, 0x7FFF, 0xFFFF};

// The vendor a rule names, or nullopt when it is not a plain hex literal (a
// glob, say) — those are reported rather than resolved.
std::optional<int> ruleVendorId(const Rule& rule) {
    int value = 0;
    const char* const last = rule.vendor.data() + rule.vendor.size();
    const auto res = std::from_chars(rule.vendor.data(), last, value, 16);
    if (res.ec != std::errc{} || res.ptr != last) { return std::nullopt; }
    return value;
}

// True when the code hands this vid:pid a decoder, or auto-claims it Direct,
// without the model appearing in kKnownModels.
bool claimedByVendorLane(const HidrawGateway& gateway, int vid, int pid) {
    return gateway.isKnownFastLaneModel(vid, pid) ||
           parserForDevice(vid, pid) != HidParser::GenericHid;
}

} // namespace

TEST_CASE("udev rules cover every model in the known-model table", "[udev]") {
    const auto rules = readRules();
    REQUIRE_FALSE(rules.empty());

    for (const auto& model : kKnownModels) {
        INFO(idText(model.vid) << ":" << idText(model.pid) << " " << model.name);
        CHECK(covers(rules, model.vid, model.pid));
    }
}

TEST_CASE("udev rules cover every vendor the code claims without a table row", "[udev]") {
    const auto rules = readRules();
    REQUIRE_FALSE(rules.empty());

    // The probes stand for unlisted products, so a table row would defeat them.
    for (const int pid : kUnlistedPids) {
        for (const auto& model : kKnownModels) { REQUIRE(model.pid != pid); }
    }

    const HidrawGateway gateway;
    std::vector<int> vendorLanes;
    for (int vid = 0; vid <= 0xFFFF; vid++) {
        for (const int pid : kUnlistedPids) {
            if (!claimedByVendorLane(gateway, vid, pid)) { continue; }
            INFO("vendor " << idText(vid) << " claims unlisted product " << idText(pid));
            CHECK(coversWholeVendor(rules, vid));
            if (vendorLanes.empty() || vendorLanes.back() != vid) { vendorLanes.push_back(vid); }
        }
    }
    // A scan that found nothing would pass the loop above vacuously.
    CHECK_FALSE(vendorLanes.empty());
}

TEST_CASE("udev rules grant access with ids spelled the way sysfs publishes them", "[udev]") {
    const auto rules = readRules();
    REQUIRE_FALSE(rules.empty());

    // sysfs prints idVendor/idProduct with %04x and udev matches ATTRS values
    // case-sensitively, so an uppercase rule would silently never fire.
    for (const Rule& rule : rules) {
        INFO("rules line " << rule.line);
        CHECK(rule.vendor == lowerHex(rule.vendor));
        CHECK(rule.product == lowerHex(rule.product));
        CHECK(rule.grantsAccess);
    }
}

TEST_CASE("udev rules apply to hidraw nodes only", "[udev]") {
    const std::string text = rulesText();

    // Without the guard the vendor-wide lines would also loosen every other
    // node the same device publishes, and udev drops a file whose GOTO has no
    // matching LABEL, which would take the whole feature down with it.
    CHECK(text.find("KERNEL!=\"hidraw*\"") != std::string::npos);
    const auto target = quotedValue(text, "GOTO=");
    REQUIRE(target.has_value());
    CHECK(text.find("LABEL=\"" + *target + "\"") != std::string::npos);
}

TEST_CASE("udev rules naming a model the code never claims only warn", "[udev]") {
    const auto rules = readRules();
    REQUIRE_FALSE(rules.empty());

    // This direction is deliberately not gated. A rule for a model nothing
    // claims grants access to a node Dish never opens, which costs nothing and
    // is how a pad gets packaged one release ahead of its parser. Failing on it
    // would make the rules file lag the code instead of lead it.
    const HidrawGateway gateway;
    for (const Rule& rule : rules) {
        const auto vid = ruleVendorId(rule);
        bool claimed = false;
        for (const int pid : kUnlistedPids) {
            claimed = claimed || (vid && claimedByVendorLane(gateway, *vid, pid));
        }
        for (const auto& model : kKnownModels) {
            if (!vid || model.vid != *vid) { continue; }
            claimed = claimed || rule.product.empty() || rule.product == idText(model.pid);
        }
        if (claimed) { continue; }
        WARN("rules line " << rule.line << " covers " << rule.vendor << ":"
                           << (rule.product.empty() ? std::string("*") : rule.product)
                           << ", which no parser or fast lane claims");
    }
}
