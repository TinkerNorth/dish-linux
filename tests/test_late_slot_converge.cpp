// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Diffs the WHOLE descriptor, not just (ctrlIdx,type): a touchpad-mode flip or
// a caps change on the same index is a resync too.

#include "core/model/Protocol.h"
#include "core/reducer/LateSlotConverge.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace reducer = dish::reducer;
namespace proto = dish::proto;
using reducer::DescriptorSlot;
using reducer::LateConvergeDesc;
using reducer::lateSlotConvergeDesc;

namespace {

DescriptorSlot desc(std::uint8_t idx, std::uint8_t type = proto::kControllerTypeXbox,
                    std::uint16_t caps = proto::kCapRumble,
                    std::uint8_t mode = proto::kTouchpadModeOff) {
    DescriptorSlot d;
    d.ctrlIdx = idx;
    d.type = type;
    d.caps = caps;
    d.touchpadMode = mode;
    return d;
}

const std::vector<std::uint8_t> kNone{};

} // namespace

TEST_CASE("lateSlotConvergeDesc: identical sent and desired need no converge", "[lateconverge]") {
    const std::vector<DescriptorSlot> set = {desc(0), desc(1, 1)};
    REQUIRE(lateSlotConvergeDesc(set, set) == LateConvergeDesc{kNone, kNone});
}

TEST_CASE("lateSlotConvergeDesc: empty sent and desired need no converge", "[lateconverge]") {
    REQUIRE(lateSlotConvergeDesc({}, {}) == LateConvergeDesc{kNone, kNone});
}

TEST_CASE("lateSlotConvergeDesc: a slot the PUT never carried is resynced", "[lateconverge]") {
    const auto c = lateSlotConvergeDesc(/*sent=*/{}, /*desired=*/{desc(0, 1)});
    REQUIRE(c.resyncs == std::vector<std::uint8_t>{0});
    REQUIRE(c.deletes == kNone);
}

TEST_CASE("lateSlotConvergeDesc: a type changed since the snapshot is resynced", "[lateconverge]") {
    const auto c = lateSlotConvergeDesc({desc(0, 0)}, {desc(0, 1)});
    REQUIRE(c.resyncs == std::vector<std::uint8_t>{0});
}

TEST_CASE("lateSlotConvergeDesc: a touchpad mode changed since the snapshot is resynced",
          "[lateconverge]") {
    const auto c = lateSlotConvergeDesc({desc(0, 0, proto::kCapRumble, proto::kTouchpadModeOff)},
                                        {desc(0, 0, proto::kCapRumble, proto::kTouchpadModeDs4)});
    REQUIRE(c.resyncs == std::vector<std::uint8_t>{0});
}

TEST_CASE("lateSlotConvergeDesc: caps changed since the snapshot are resynced", "[lateconverge]") {
    const auto c = lateSlotConvergeDesc({desc(0, 0, proto::kCapRumble)},
                                        {desc(0, 0, proto::kCapRumble | proto::kCapMotion)});
    REQUIRE(c.resyncs == std::vector<std::uint8_t>{0});
}

TEST_CASE("lateSlotConvergeDesc: a slot removed since the snapshot is deleted", "[lateconverge]") {
    const auto c = lateSlotConvergeDesc({desc(0)}, {});
    REQUIRE(c.resyncs == kNone);
    REQUIRE(c.deletes == std::vector<std::uint8_t>{0});
}

TEST_CASE("lateSlotConvergeDesc: an index re-used with an identical descriptor is left alone",
          "[lateconverge]") {
    const auto c = lateSlotConvergeDesc({desc(0, 1)}, {desc(0, 1)});
    REQUIRE(c == LateConvergeDesc{kNone, kNone});
}

TEST_CASE("lateSlotConvergeDesc: an index re-used with a different descriptor resyncs not deletes",
          "[lateconverge]") {
    const auto c = lateSlotConvergeDesc({desc(0, 0)}, {desc(0, 1)});
    REQUIRE(c.resyncs == std::vector<std::uint8_t>{0});
    REQUIRE(c.deletes == kNone);
}

TEST_CASE("lateSlotConvergeDesc: adds changes and removes resolve independently",
          "[lateconverge]") {
    const auto c = lateSlotConvergeDesc(
        /*sent=*/{desc(0, 0), desc(1, 1), desc(2, 0)},
        /*desired=*/{desc(0, 0), desc(1, 0), desc(3, 1)});
    REQUIRE(c.resyncs == std::vector<std::uint8_t>{1, 3});
    REQUIRE(c.deletes == std::vector<std::uint8_t>{2});
}
