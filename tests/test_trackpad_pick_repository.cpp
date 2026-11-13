// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The ladder owns the trackpad pick's default per model, so this layer reads
// back nullopt for an unwritten slot rather than a pick.

#include "repository/TrackpadPickRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::repository::TrackpadPickPreference;
using dish::repository::TrackpadPickRepository;
using dish::test::makeSharedSettings;

TEST_CASE("TrackpadPickRepository satisfies the repository contract",
          "[repository][trackpad-pick]") {
    dish::test::runRepositoryContract<QString, TrackpadPickPreference>(
        [] { return std::make_unique<TrackpadPickRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("slot-%1").arg(i); },
        [](const QString& k) {
            return TrackpadPickPreference{k, (k.size() % 2) == 0 ? QStringLiteral("pad")
                                                                 : QStringLiteral("stick")};
        });
}

TEST_CASE("trackpad picks round-trip with durability", "[trackpad-pick-repo]") {
    auto store = makeSharedSettings();
    {
        TrackpadPickRepository repo(store);
        repo.put(TrackpadPickPreference{"685642050", "pad"});
        repo.put(TrackpadPickPreference{"685642050:2", "off"});
    }
    TrackpadPickRepository repo2(store);
    REQUIRE(repo2.get("685642050").has_value());
    CHECK(repo2.get("685642050")->pick == "pad");
    REQUIRE(repo2.get("685642050:2").has_value());
    CHECK(repo2.get("685642050:2")->pick == "off");
}

TEST_CASE("get on a never-picked slot returns nullopt, not a default", "[trackpad-pick-repo]") {
    TrackpadPickRepository repo(makeSharedSettings());
    CHECK_FALSE(repo.get("never-picked").has_value());
}

TEST_CASE("a pick that is not one of the four is refused at the door", "[trackpad-pick-repo]") {
    TrackpadPickRepository repo(makeSharedSettings());
    repo.put(TrackpadPickPreference{"slot", "stick"});
    repo.put(TrackpadPickPreference{"slot", "ds4"});
    repo.put(TrackpadPickPreference{"other", "banana"});
    REQUIRE(repo.get("slot").has_value());
    CHECK(repo.get("slot")->pick == "stick");
    CHECK_FALSE(repo.get("other").has_value());
}

TEST_CASE("corrupt payload falls back to empty, does not crash", "[trackpad-pick-repo]") {
    auto store = makeSharedSettings();
    store->setValue(QLatin1String(dish::repository::kTrackpadPickListKey),
                    QByteArrayLiteral("{not valid json"));
    TrackpadPickRepository repo(store);
    CHECK(repo.all().empty());
    CHECK_FALSE(repo.get("anything").has_value());
}

TEST_CASE("remove of one slot does not disturb the others", "[trackpad-pick-repo]") {
    TrackpadPickRepository repo(makeSharedSettings());
    repo.put(TrackpadPickPreference{"keep", "pad"});
    repo.put(TrackpadPickPreference{"drop", "mouse"});
    repo.remove(QStringLiteral("drop"));
    CHECK_FALSE(repo.get("drop").has_value());
    REQUIRE(repo.get("keep").has_value());
    CHECK(repo.get("keep")->pick == "pad");
}

TEST_CASE("put on the same slot replaces in place", "[trackpad-pick-repo]") {
    TrackpadPickRepository repo(makeSharedSettings());
    repo.put(TrackpadPickPreference{"slot", "off"});
    repo.put(TrackpadPickPreference{"slot", "pad"});
    REQUIRE(repo.all().size() == 1);
    CHECK(repo.get("slot")->pick == "pad");
}
