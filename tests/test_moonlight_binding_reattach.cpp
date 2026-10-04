// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which standing Moonlight bindings the app model puts back when the slot list moves.

#include "source/moonlight/MoonlightBindingReattach.h"

#include <catch2/catch_test_macros.hpp>

#include <QSet>
#include <QString>

#include <vector>

using dish::repository::MoonlightBinding;
using dish::source::moon::bindingsToReattach;

namespace {

const QString kPad = QStringLiteral("sdl:1");
const QString kOtherPad = QStringLiteral("sdl:2");
const QString kHost = QStringLiteral("host-a");

MoonlightBinding standing(const QString& slot) {
    MoonlightBinding binding;
    binding.slotId = slot;
    binding.hostUuid = kHost;
    return binding;
}

const QSet<QString> kNone;

} // namespace

TEST_CASE("a standing binding whose pad is here and drives nothing is put back",
          "[moonlight][binding][reattach]") {
    const auto plan = bindingsToReattach({standing(kPad)}, {kPad}, kNone, kNone, kNone);
    REQUIRE(plan.size() == 1);
    CHECK(plan.front().slotId == kPad);
    CHECK(plan.front().hostUuid == kHost);
}

TEST_CASE("a pad that is not here, already drives its host, or rides a satellite is left alone",
          "[moonlight][binding][reattach]") {
    CHECK(bindingsToReattach({standing(kPad)}, kNone, kNone, kNone, kNone).empty());
    CHECK(bindingsToReattach({standing(kPad)}, {kPad}, {kPad}, kNone, kNone).empty());
    CHECK(bindingsToReattach({standing(kPad)}, {kPad}, kNone, {kPad}, kNone).empty());
}

TEST_CASE("a binding already tried since the pad appeared is not asked again",
          "[moonlight][binding][reattach]") {
    CHECK(bindingsToReattach({standing(kPad)}, {kPad}, kNone, kNone, {kPad}).empty());
}

TEST_CASE("only the bindings whose pads are here are put back", "[moonlight][binding][reattach]") {
    const auto plan =
        bindingsToReattach({standing(kPad), standing(kOtherPad)}, {kOtherPad}, kNone, kNone, kNone);
    REQUIRE(plan.size() == 1);
    CHECK(plan.front().slotId == kOtherPad);
}
