// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The descriptor parse at ITEM level, one rule per case.
//
// test_usb_hid_layout.cpp parses whole well-formed descriptors and checks the
// field map that comes out. These cases go at the rules between: what a long
// item does, what a truncated stream does, when the report id resets the bit
// cursor, which report locks the layout, and how a usage list shorter than the
// report count is read. Each is a branch the item functions carry that a
// well-formed two-stick pad never reaches.

#include "core/input/UsbHidLayout.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using dish::input::usbhid::HidLayout;
using dish::input::usbhid::parseReportDescriptor;

namespace {

// Descriptor building blocks, so each case reads as the rule it is about rather
// than as a wall of hex.
constexpr std::uint8_t kUsagePageDesktop[] = {0x05, 0x01};
constexpr std::uint8_t kUsagePageButton[] = {0x05, 0x09};
constexpr std::uint8_t kUsageGamePad[] = {0x09, 0x05};
constexpr std::uint8_t kCollectionApp[] = {0xA1, 0x01};
constexpr std::uint8_t kEndCollection[] = {0xC0};
constexpr std::uint8_t kUsageX[] = {0x09, 0x30};
constexpr std::uint8_t kUsageY[] = {0x09, 0x31};
constexpr std::uint8_t kLogicalMin0[] = {0x15, 0x00};
constexpr std::uint8_t kLogicalMax255[] = {0x26, 0xFF, 0x00};
constexpr std::uint8_t kLogicalMax1[] = {0x25, 0x01};
constexpr std::uint8_t kReportSize8[] = {0x75, 0x08};
constexpr std::uint8_t kReportSize1[] = {0x75, 0x01};
constexpr std::uint8_t kInputData[] = {0x81, 0x02};
constexpr std::uint8_t kInputConst[] = {0x81, 0x01};

class Descriptor {
  public:
    template <std::size_t N> Descriptor& operator<<(const std::uint8_t (&bytes)[N]) {
        bytes_.insert(bytes_.end(), bytes, bytes + N);
        return *this;
    }

    Descriptor& raw(std::initializer_list<std::uint8_t> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return *this;
    }

    Descriptor& reportId(std::uint8_t id) { return raw({0x85, id}); }
    Descriptor& reportCount(std::uint8_t n) { return raw({0x95, n}); }
    Descriptor& usageMinMax(std::uint8_t lo, std::uint8_t hi) { return raw({0x19, lo, 0x29, hi}); }

    const std::uint8_t* data() const { return bytes_.data(); }
    std::size_t size() const { return bytes_.size(); }

  private:
    std::vector<std::uint8_t> bytes_;
};

// The preamble every descriptor below shares.
Descriptor gamepadHeader() {
    Descriptor d;
    d << kUsagePageDesktop << kUsageGamePad << kCollectionApp << kUsagePageDesktop;
    return d;
}

// Two 8-bit axes, which is the smallest thing that parses to a valid layout.
void appendTwoAxes(Descriptor& d) {
    d << kUsageX << kUsageY << kLogicalMin0 << kLogicalMax255 << kReportSize8;
    d.reportCount(2);
    d << kInputData;
}

HidLayout parse(const Descriptor& d) {
    HidLayout out;
    parseReportDescriptor(d.data(), d.size(), out);
    return out;
}

} // namespace

TEST_CASE("hid items: a long item is stepped over whole", "[hid][items]") {
    // 0xFE is a long item: one payload-length byte, one tag byte, then the
    // payload. No gamepad descriptor uses one, and skipping it wrongly would
    // make the axes that follow unreadable.
    Descriptor d = gamepadHeader();
    d.raw({0xFE, 0x03, 0x00, 0xDE, 0xAD, 0xBE}); // length 3, tag 0, three bytes
    appendTwoAxes(d);
    d << kEndCollection;

    const HidLayout out = parse(d);
    CHECK(out.valid);
    CHECK(out.lx.present);
    CHECK(out.ly.present);
    CHECK(out.lx.bitOffset == 0);
    CHECK(out.ly.bitOffset == 8);
}

TEST_CASE("hid items: a long item whose length byte is missing ends the parse", "[hid][items]") {
    // Truncated mid-item. Whatever was read before it stands; nothing after is
    // invented.
    Descriptor d = gamepadHeader();
    appendTwoAxes(d);
    d.raw({0xFE}); // the length byte never arrives

    const HidLayout out = parse(d);
    CHECK(out.valid);
    CHECK(out.lx.present);
    CHECK(out.ly.present);
}

TEST_CASE("hid items: a short item whose data is cut short ends the parse", "[hid][items]") {
    // A 4-byte item prefix (bSize 3) with only two bytes behind it. The axes
    // already read stay; the button block that would have followed does not
    // appear from nowhere.
    Descriptor d = gamepadHeader();
    appendTwoAxes(d);
    d.raw({0x27, 0xFF, 0xFF}); // Logical Maximum, 4 data bytes declared, 2 given

    const HidLayout out = parse(d);
    CHECK(out.valid);
    CHECK(out.lx.present);
    CHECK(out.buttonCount == 0);
}

TEST_CASE("hid items: an empty descriptor yields nothing and says so", "[hid][items]") {
    HidLayout out;
    const std::uint8_t nothing[] = {0x00};
    CHECK_FALSE(parseReportDescriptor(nothing, 0, out));
    CHECK_FALSE(out.valid);
}

TEST_CASE("hid items: constant padding moves the cursor without taking a field", "[hid][items]") {
    // One byte of Input(Const) between X and Y. Y has to land at bit 16, not
    // bit 8: padding carries no field but still occupies the report.
    Descriptor d = gamepadHeader();
    d << kUsageX << kLogicalMin0 << kLogicalMax255 << kReportSize8;
    d.reportCount(1);
    d << kInputData;
    d << kReportSize8;
    d.reportCount(1);
    d << kInputConst;
    d << kUsageY << kReportSize8;
    d.reportCount(1);
    d << kInputData;
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    CHECK(out.lx.bitOffset == 0);
    CHECK(out.ly.bitOffset == 16);
}

TEST_CASE("hid items: a report id is carried into the layout", "[hid][items]") {
    // The layout has to know whether the device sends a prefix byte, because
    // every offset below is relative to the payload after it.
    Descriptor d = gamepadHeader();
    d.reportId(3);
    appendTwoAxes(d);
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    CHECK(out.reportId == 3);
    CHECK(out.lx.bitOffset == 0);
    CHECK(out.ly.bitOffset == 8);
}

TEST_CASE("hid items: a report id item resets the bit cursor", "[hid][items]") {
    // Two Input blocks under the SAME id, with the id restated between them.
    // The cursor resets on the restatement, so Y is placed at bit 0 rather than
    // continuing at bit 8.
    //
    // This is the parser's simplifying assumption, not the HID spec's rule: the
    // spec accumulates per report id, so a descriptor that interleaves or
    // restates ids would want a cursor per id. Every gamepad descriptor this
    // decodes writes each report once and contiguously, which is why the single
    // cursor holds. The case is here so the assumption is visible rather than
    // implicit, and so a future per-id cursor changes a test on purpose.
    Descriptor d = gamepadHeader();
    d.reportId(3);
    d << kUsageX << kLogicalMin0 << kLogicalMax255 << kReportSize8;
    d.reportCount(1);
    d << kInputData;
    d.reportId(3);
    d << kUsageY << kReportSize8;
    d.reportCount(1);
    d << kInputData;
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    CHECK(out.reportId == 3);
    CHECK(out.lx.bitOffset == 0);
    CHECK(out.ly.bitOffset == 0);
}

TEST_CASE("hid items: the first report that carries fields locks the layout", "[hid][items]") {
    // A device with two input reports is described by the one this parse can
    // decode, not by a mixture of both. Report 3 has the axes; report 4's
    // buttons must not be folded into the same map.
    Descriptor d = gamepadHeader();
    d.reportId(3);
    appendTwoAxes(d);
    d.reportId(4);
    d << kUsagePageButton;
    d.usageMinMax(1, 8);
    d << kLogicalMin0 << kLogicalMax1 << kReportSize1;
    d.reportCount(8);
    d << kInputData;
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    CHECK(out.reportId == 3);
    CHECK(out.lx.present);
    CHECK(out.buttonCount == 0);
}

TEST_CASE("hid items: the first button run wins", "[hid][items]") {
    // A descriptor that declares a second button block is describing something
    // else; the layout carries one run.
    Descriptor d = gamepadHeader();
    appendTwoAxes(d);
    d << kUsagePageButton;
    d.usageMinMax(1, 4);
    d << kLogicalMin0 << kLogicalMax1 << kReportSize1;
    d.reportCount(4);
    d << kInputData;
    d.usageMinMax(1, 8);
    d.reportCount(8);
    d << kInputData;
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    CHECK(out.buttonCount == 4);
    CHECK(out.buttonBitOffset == 16);
}

TEST_CASE("hid items: an axis keeps the first offset it was given", "[hid][items]") {
    // Two usages declared, three fields asked for. The list is short, so the
    // third field repeats the last usage (Y) rather than reading off the end of
    // it -- and because an axis that is already placed keeps its offset, that
    // repeat assigns nothing. The two rules together are why a descriptor whose
    // report count overruns its usage list still decodes correctly.
    Descriptor d = gamepadHeader();
    d << kUsageX << kUsageY << kLogicalMin0 << kLogicalMax255 << kReportSize8;
    d.reportCount(3);
    d << kInputData;
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    CHECK(out.lx.present);
    CHECK(out.ly.present);
    CHECK(out.lx.bitOffset == 0);
    CHECK(out.ly.bitOffset == 8);
}

TEST_CASE("hid items: a usage declared twice does not move", "[hid][items]") {
    // The same first-wins rule, stated on its own: X named in two separate
    // Input blocks is read from the first one's bits.
    Descriptor d = gamepadHeader();
    d << kUsageX << kLogicalMin0 << kLogicalMax255 << kReportSize8;
    d.reportCount(1);
    d << kInputData;
    d << kUsageX << kReportSize8;
    d.reportCount(1);
    d << kInputData;
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    CHECK(out.lx.bitOffset == 0);
}

TEST_CASE("hid items: an axis block with no usages at all takes no fields", "[hid][items]") {
    // A Main item with a report size and count but no Local usage items and no
    // range: there is nothing to assign, and the parse must not invent one.
    Descriptor d = gamepadHeader();
    d << kLogicalMin0 << kLogicalMax255 << kReportSize8;
    d.reportCount(2);
    d << kInputData;
    appendTwoAxes(d);
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    // The named axes are the ones that followed, placed after the anonymous
    // block's 16 bits.
    CHECK(out.lx.bitOffset == 16);
    CHECK(out.ly.bitOffset == 24);
}

TEST_CASE("hid items: a usage range counts up from its minimum", "[hid][items]") {
    // Usage Minimum 0x30 (X) with a report count of 2 gives X then Y, without
    // either being listed.
    Descriptor d = gamepadHeader();
    d.raw({0x1A, 0x30, 0x00}); // Usage Minimum (0x0030)
    d.raw({0x2A, 0x31, 0x00}); // Usage Maximum (0x0031)
    d << kLogicalMin0 << kLogicalMax255 << kReportSize8;
    d.reportCount(2);
    d << kInputData;
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    CHECK(out.lx.present);
    CHECK(out.ly.present);
    CHECK(out.lx.bitOffset == 0);
    CHECK(out.ly.bitOffset == 8);
}

TEST_CASE("hid items: local items are cleared by the Main item that consumes them",
          "[hid][items]") {
    // X and Y are declared before the first Input. The second Input names no
    // usages of its own, so it must assign none rather than re-using the pair
    // the first one consumed.
    Descriptor d = gamepadHeader();
    appendTwoAxes(d);
    d << kReportSize8;
    d.reportCount(2);
    d << kInputData;
    d << kEndCollection;

    const HidLayout out = parse(d);
    REQUIRE(out.valid);
    CHECK(out.lx.bitOffset == 0);
    CHECK(out.ly.bitOffset == 8);
}
