// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which standing Moonlight bindings the app model puts back when the slot list moves, and under
// which slot: a pad is followed by its identity, and SDL's instance id is the order pads were
// plugged, not the pad.

#include "source/moonlight/MoonlightBindingReattach.h"

#include <catch2/catch_test_macros.hpp>

#include <QSet>
#include <QString>

#include <initializer_list>
#include <vector>

using dish::repository::MoonlightBinding;
using dish::source::moon::bindingsToReattach;
using dish::source::moon::PresentPad;

namespace {

const QString kPad = QStringLiteral("sdl:1");
const QString kOtherPad = QStringLiteral("sdl:2");
const QString kHost = QStringLiteral("host-a");
const QString kDualSense = QStringLiteral("guid:0300aabb/serial:11:22:33");
const QString kSecondDualSense = QStringLiteral("guid:0300aabb/serial:44:55:66");

MoonlightBinding standing(const QString& slot, const QString& identity = QString()) {
    MoonlightBinding binding;
    binding.slotId = slot;
    binding.hostUuid = kHost;
    binding.padIdentity = identity;
    return binding;
}

std::vector<PresentPad> here(std::initializer_list<PresentPad> pads) { return pads; }

const QSet<QString> kNone;

} // namespace

TEST_CASE("a record without an identity follows its slot id and learns the pad's identity",
          "[moonlight][binding][reattach]") {
    const auto plan =
        bindingsToReattach({standing(kPad)}, here({{kPad, kDualSense}}), kNone, kNone, kNone);
    REQUIRE(plan.size() == 1);
    CHECK(plan.front().binding.slotId == kPad);
    CHECK(plan.front().binding.hostUuid == kHost);
    CHECK(plan.front().slotId == kPad);
    CHECK(plan.front().identity == kDualSense);
}

TEST_CASE("a record with an identity follows its pad to the slot it holds now",
          "[moonlight][binding][reattach]") {
    const auto plan = bindingsToReattach({standing(kPad, kDualSense)},
                                         here({{kOtherPad, kDualSense}}), kNone, kNone, kNone);
    REQUIRE(plan.size() == 1);
    CHECK(plan.front().binding.slotId == kPad);
    CHECK(plan.front().slotId == kOtherPad);
    CHECK(plan.front().identity == kDualSense);
}

TEST_CASE("a record with an identity ignores another pad that took its old slot id",
          "[moonlight][binding][reattach]") {
    CHECK(bindingsToReattach({standing(kPad, kDualSense)}, here({{kPad, kSecondDualSense}}), kNone,
                             kNone, kNone)
              .empty());
}

TEST_CASE("two records of one identity take two pads, never the same one",
          "[moonlight][binding][reattach]") {
    const auto plan = bindingsToReattach(
        {standing(kPad, kDualSense), standing(kOtherPad, kDualSense)},
        here({{kOtherPad, kDualSense}, {kPad, kDualSense}}), kNone, kNone, kNone);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].slotId == kOtherPad);
    CHECK(plan[1].slotId == kPad);
}

TEST_CASE("a pad that is not here, already drives its host, or rides a satellite is left alone",
          "[moonlight][binding][reattach]") {
    CHECK(bindingsToReattach({standing(kPad)}, {}, kNone, kNone, kNone).empty());
    CHECK(bindingsToReattach({standing(kPad)}, here({{kPad, {}}}), {kPad}, kNone, kNone).empty());
    CHECK(bindingsToReattach({standing(kPad)}, here({{kPad, {}}}), kNone, {kPad}, kNone).empty());
}

TEST_CASE("the checks apply to the slot the pad holds now, not the one the record names",
          "[moonlight][binding][reattach]") {
    const auto standingPad = standing(kPad, kDualSense);
    const auto present = here({{kOtherPad, kDualSense}});
    CHECK(bindingsToReattach({standingPad}, present, {kOtherPad}, kNone, kNone).empty());
    CHECK(bindingsToReattach({standingPad}, present, kNone, {kOtherPad}, kNone).empty());
    CHECK(bindingsToReattach({standingPad}, present, kNone, kNone, {kOtherPad}).empty());
    CHECK(bindingsToReattach({standingPad}, present, {kPad}, {kPad}, {kPad}).size() == 1);
}

TEST_CASE("a binding already tried since the pad appeared is not asked again",
          "[moonlight][binding][reattach]") {
    CHECK(bindingsToReattach({standing(kPad)}, here({{kPad, {}}}), kNone, kNone, {kPad}).empty());
}

TEST_CASE("only the bindings whose pads are here are put back", "[moonlight][binding][reattach]") {
    const auto plan = bindingsToReattach({standing(kPad), standing(kOtherPad)},
                                         here({{kOtherPad, {}}}), kNone, kNone, kNone);
    REQUIRE(plan.size() == 1);
    CHECK(plan.front().slotId == kOtherPad);
}
