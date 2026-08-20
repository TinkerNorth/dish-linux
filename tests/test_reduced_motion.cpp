// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The two live probes talk to the session bus and to kdeglobals, so precedence
// is exercised through the injectable seam instead: TokensBridge feeds it the
// real probes, these feed it scripted ones.

#include "qml/chrome/TokensBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QVariant>

#include <optional>

using dish::chrome::kdeAnimationsEnabledFor;
using dish::chrome::MotionPreference;
using dish::chrome::reducedMotionFrom;

namespace {

// Counts its own calls so "asked" and "not asked" are observable, rather than
// inferred from an answer either source could have given.
class Probe {
  public:
    explicit Probe(std::optional<bool> animationsEnabled) : answer_(animationsEnabled) {}

    std::optional<bool> operator()() {
        ++calls_;
        return answer_;
    }

    int calls() const { return calls_; }

  private:
    std::optional<bool> answer_;
    int calls_ = 0;
};

// The whole KDE arm of the ladder, from the raw ini value to the preference.
bool reducedForFactor(const char* durationFactor) {
    Probe portal{std::nullopt};
    Probe kde{kdeAnimationsEnabledFor(QVariant(QString::fromUtf8(durationFactor)))};
    return reducedMotionFrom(portal, kde);
}

} // namespace

TEST_CASE("the portal's answer wins over KDE's", "[chrome][reduced-motion]") {
    Probe portalOff{false};
    Probe kdeOn{true};
    REQUIRE(reducedMotionFrom(portalOff, kdeOn));

    Probe portalOn{true};
    Probe kdeOff{false};
    REQUIRE_FALSE(reducedMotionFrom(portalOn, kdeOff));
}

TEST_CASE("KDE is not consulted once the portal has answered", "[chrome][reduced-motion]") {
    Probe portal{true};
    Probe kde{false};
    reducedMotionFrom(portal, kde);
    CHECK(portal.calls() == 1);
    CHECK(kde.calls() == 0);
}

TEST_CASE("KDE is consulted when the portal stays silent", "[chrome][reduced-motion]") {
    Probe portal{std::nullopt};
    Probe kde{false};
    REQUIRE(reducedMotionFrom(portal, kde));
    CHECK(portal.calls() == 1);
    CHECK(kde.calls() == 1);
}

TEST_CASE("neither source answering leaves motion allowed", "[chrome][reduced-motion]") {
    Probe portal{std::nullopt};
    Probe kde{std::nullopt};
    REQUIRE_FALSE(reducedMotionFrom(portal, kde));
    CHECK(kde.calls() == 1);
}

TEST_CASE("only a zero AnimationDurationFactor reduces motion", "[chrome][reduced-motion]") {
    CHECK(reducedForFactor("0"));
    CHECK_FALSE(reducedForFactor("0.5"));
    CHECK_FALSE(reducedForFactor("1"));
}

TEST_CASE("a non-numeric AnimationDurationFactor leaves motion allowed",
          "[chrome][reduced-motion]") {
    // QVariant::toDouble reports 0.0 on failure, which would otherwise read a
    // malformed value as a stated "animations off".
    CHECK_FALSE(kdeAnimationsEnabledFor(QVariant(QStringLiteral("nonsense"))).has_value());
    CHECK_FALSE(reducedForFactor("nonsense"));
    CHECK_FALSE(reducedForFactor(""));
}

TEST_CASE("an absent AnimationDurationFactor is not an answer", "[chrome][reduced-motion]") {
    CHECK_FALSE(kdeAnimationsEnabledFor(QVariant()).has_value());
}

TEST_CASE("the motion preference reports a change only when the value moves",
          "[chrome][reduced-motion]") {
    // refreshMotionPreference emits exactly when this reports true.
    MotionPreference motion{false};
    CHECK_FALSE(motion.update(false));
    CHECK_FALSE(motion.reduced());

    REQUIRE(motion.update(true));
    CHECK(motion.reduced());
    CHECK_FALSE(motion.update(true));
    CHECK(motion.reduced());

    REQUIRE(motion.update(false));
    CHECK_FALSE(motion.reduced());
}
