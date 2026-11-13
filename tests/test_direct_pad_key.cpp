// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/BindingPresence.h"
#include "core/reducer/DirectPadKey.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace dish::reducer;

namespace {

constexpr int kValve = 0x28DE;
constexpr int kReceiver2015 = 0x1142;
// 0x28DE1142, the 2015 receiver's model key.
const std::string kReceiverModelKey = "685642050";

} // namespace

TEST_CASE("a model's own pad keeps the decimal model key it always had", "[directpad]") {
    const DirectPadKey dualSense{0x054C, 0x0CE6};
    CHECK(directSlotId(dualSense) == std::to_string(slotPathVpKey(0x054C, 0x0CE6)));
    CHECK(parseDirectSlotId(directSlotId(dualSense)) == dualSense);
    CHECK_FALSE(isReceiverSlot(dualSense));
}

TEST_CASE("a receiver's pad is its model key and the interface its slot rides", "[directpad]") {
    const DirectPadKey slot{kValve, kReceiver2015, 3};
    CHECK(directSlotId(slot) == kReceiverModelKey + ":3");
    CHECK(parseDirectSlotId(kReceiverModelKey + ":3") == slot);
    CHECK(isReceiverSlot(slot));
    CHECK(isPadOfModel(slot, kValve, kReceiver2015));
}

TEST_CASE("a model's later device writes its unit after the model key, its first keeps its id",
          "[directpad]") {
    const std::vector<std::string> ids{"88870118#1", kReceiverModelKey + "#1:3",
                                       kReceiverModelKey + "#2:1"};
    for (const std::string& id : ids) {
        INFO(id);
        const auto pad = parseDirectSlotId(id);
        REQUIRE(pad.has_value());
        CHECK(directSlotId(*pad) == id);
    }
    CHECK(parseDirectSlotId(kReceiverModelKey + "#1:3") ==
          DirectPadKey{kValve, kReceiver2015, 3, 1});
    CHECK(directSlotId(DirectPadKey{kValve, kReceiver2015, 3, 0}) == kReceiverModelKey + ":3");
}

TEST_CASE("an SDL id, a malformed slot or junk is no Direct pad", "[directpad]") {
    const std::vector<std::string> ids{"sdl:3",
                                       kReceiverModelKey + ":",
                                       kReceiverModelKey + ":x",
                                       ":2",
                                       "",
                                       kReceiverModelKey + ":2:3",
                                       kReceiverModelKey + ":256",
                                       kReceiverModelKey + ":1000",
                                       "-5",
                                       kReceiverModelKey + "#0",
                                       kReceiverModelKey + "#0:2",
                                       kReceiverModelKey + "#",
                                       kReceiverModelKey + "#x",
                                       kReceiverModelKey + "#256",
                                       kReceiverModelKey + "#1#2",
                                       kReceiverModelKey + ":2#1",
                                       "#1"};
    for (const std::string& id : ids) {
        INFO(id);
        CHECK_FALSE(parseDirectSlotId(id).has_value());
    }
}

TEST_CASE("pads order the way the model keys always did, a receiver's slots in turn",
          "[directpad]") {
    std::vector<DirectPadKey> pads{
        {kValve, kReceiver2015, 4}, {0x2DC8, 0x3100}, {kValve, kReceiver2015, 1}, {0x054C, 0x0CE6}};
    std::sort(pads.begin(), pads.end());
    // 0x054C0CE6 < 0x28DE1142 < 0x2DC83100.
    CHECK(pads == std::vector<DirectPadKey>{{0x054C, 0x0CE6},
                                            {kValve, kReceiver2015, 1},
                                            {kValve, kReceiver2015, 4},
                                            {0x2DC8, 0x3100}});
}

TEST_CASE("a model's devices list one after the other, each one's slots in turn", "[directpad]") {
    std::vector<DirectPadKey> pads{{kValve, kReceiver2015, 1, 1},
                                   {kValve, kReceiver2015, 4, 0},
                                   {kValve, kReceiver2015, 2, 1},
                                   {kValve, kReceiver2015, 1, 0}};
    std::sort(pads.begin(), pads.end());
    CHECK(pads == std::vector<DirectPadKey>{{kValve, kReceiver2015, 1, 0},
                                            {kValve, kReceiver2015, 4, 0},
                                            {kValve, kReceiver2015, 1, 1},
                                            {kValve, kReceiver2015, 2, 1}});
}

TEST_CASE("only a receiver's pad stands for itself when a binding re-resolves its slot",
          "[directpad]") {
    CHECK(isReceiverSlotId(kReceiverModelKey + ":2"));
    CHECK_FALSE(isReceiverSlotId(kReceiverModelKey));
    CHECK_FALSE(isReceiverSlotId("sdl:3"));
    CHECK_FALSE(isReceiverSlotId(std::to_string(slotPathVpKey(0x054C, 0x0CE6))));
}

TEST_CASE("a departed model pad's binding names its model, a receiver slot's names none",
          "[directpad]") {
    CHECK(departedPadIdentity(DirectPadKey{0x054C, 0x0CE6}) == std::make_pair(0x054C, 0x0CE6));
    CHECK_FALSE(departedPadIdentity(DirectPadKey{kValve, kReceiver2015, 2}).has_value());
}

TEST_CASE("a receiver slot's binding leaves with its controller instead of moving to a sibling",
          "[directpad][bindingpresence]") {
    // Slot 2's controller turned off while slot 3's stays on: a binding that
    // followed the model would land on someone else's controller.
    const std::string departed = kReceiverModelKey + ":2";
    const std::string sibling = kReceiverModelKey + ":3";
    const std::vector<PresentSlot> present{{sibling, kValve, kReceiver2015}};
    const std::vector<BoundSlot> bindings{
        {departed, "sat-1", departedPadIdentity(*parseDirectSlotId(departed))}};

    const auto actions = resolveBindingPresence(present, bindings);

    REQUIRE(actions.size() == 1U);
    CHECK(actions.front().kind == BindingPresenceKind::Unbind);
    CHECK(actions.front().slotId == departed);
}
