// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The migrations here come from reducer::resolveBindingPresence, the way AppModel
// gets them: a claim hides the pad's SDL slot and shows its Direct one, and a
// release does the reverse.

#include "core/reducer/BindingPresence.h"
#include "core/reducer/SlotPathFields.h"
#include "repository/AudioPreferenceRepository.h"
#include "repository/MotionPreferenceRepository.h"
#include "repository/RumblePreferenceRepository.h"
#include "source/store/SlotSwitches.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>

#include <memory>
#include <string>

using dish::reducer::BindingPresenceAction;
using dish::reducer::BindingPresenceKind;
using dish::reducer::BoundSlot;
using dish::reducer::PresentSlot;
using dish::reducer::resolveBindingPresence;
using dish::reducer::slotPathVpKey;
using dish::repository::AudioPreferenceRepository;
using dish::repository::MotionPreferenceRepository;
using dish::repository::RumblePreferenceRepository;
using dish::source::carrySlotSwitches;
using dish::source::MicEnabledStore;
using dish::source::MotionEnabledStore;
using dish::source::RumbleEnabledStore;
using dish::source::SlotSwitchStores;
using dish::source::SpeakerEnabledStore;
using dish::test::makeSharedSettings;

namespace {

constexpr int kSony = 0x054C;
constexpr int kDualSense = 0x0CE6;
const std::string kSdlSlot = "sdl:3";
const std::string kDirectSlot = std::to_string(slotPathVpKey(kSony, kDualSense));
const std::string kSatellite = "wifi:a";

// The four stores as AppModel owns them, over one settings file so a second
// instance over the same file is a restart.
struct Switches {
    explicit Switches(const std::shared_ptr<QSettings>& settings)
        : motionRepo(settings), rumbleRepo(settings),
          micRepo(QStringLiteral("mic_preferences"), settings),
          speakerRepo(QStringLiteral("speaker_preferences"), settings) {}

    SlotSwitchStores stores() { return {motion, rumble, mic, speaker}; }
    bool rumbleOn(const std::string& slotId) const {
        return rumble.isEnabled(QString::fromStdString(slotId));
    }

    MotionPreferenceRepository motionRepo;
    RumblePreferenceRepository rumbleRepo;
    AudioPreferenceRepository micRepo;
    AudioPreferenceRepository speakerRepo;
    MotionEnabledStore motion{&motionRepo};
    RumbleEnabledStore rumble{&rumbleRepo};
    MicEnabledStore mic{&micRepo};
    SpeakerEnabledStore speaker{&speakerRepo};
};

BindingPresenceAction onlyMigration(const std::string& present, const std::string& bound) {
    const auto actions =
        resolveBindingPresence({PresentSlot{present, kSony, kDualSense}},
                               {BoundSlot{bound, kSatellite, std::make_pair(kSony, kDualSense)}});
    REQUIRE(actions.size() == 1);
    REQUIRE(actions.front().kind == BindingPresenceKind::Migrate);
    return actions.front();
}

BindingPresenceAction claim() { return onlyMigration(kDirectSlot, kSdlSlot); }
BindingPresenceAction release() { return onlyMigration(kSdlSlot, kDirectSlot); }

void carry(Switches& s, const BindingPresenceAction& migration) {
    carrySlotSwitches(s.stores(), migration.slotId, migration.toSlotId);
}

} // namespace

TEST_CASE("a Direct claim carries every switch the pad had to its new slot id", "[slot-switches]") {
    Switches s(makeSharedSettings());
    s.motion.setEnabled(kSdlSlot, false);
    s.rumble.setEnabled(QString::fromStdString(kSdlSlot), false);
    s.mic.setEnabled(kSdlSlot, true);
    s.speaker.setEnabled(kSdlSlot, false);

    carry(s, claim());

    CHECK_FALSE(s.motion.isEnabled(kDirectSlot));
    CHECK_FALSE(s.rumbleOn(kDirectSlot));
    CHECK(s.mic.isEnabled(kDirectSlot));
    CHECK_FALSE(s.speaker.isEnabled(kDirectSlot));
}

TEST_CASE("a release carries what changed while Direct back to the pad's SDL slot",
          "[slot-switches]") {
    Switches s(makeSharedSettings());
    s.rumble.setEnabled(QString::fromStdString(kSdlSlot), false);
    carry(s, claim());
    s.rumble.setEnabled(QString::fromStdString(kDirectSlot), true);
    s.mic.setEnabled(kDirectSlot, true);

    carry(s, release());

    CHECK(s.rumbleOn(kSdlSlot));
    CHECK(s.mic.isEnabled(kSdlSlot));
}

TEST_CASE("a switch never touched before a claim leaves the Direct slot at its default",
          "[slot-switches]") {
    Switches s(makeSharedSettings());
    s.motion.setEnabled(kDirectSlot, false);
    s.rumble.setEnabled(QString::fromStdString(kDirectSlot), false);
    s.mic.setEnabled(kDirectSlot, true);
    s.speaker.setEnabled(kDirectSlot, false);

    carry(s, claim());

    CHECK(s.motion.isEnabled(kDirectSlot));
    CHECK(s.rumbleOn(kDirectSlot));
    CHECK_FALSE(s.mic.isEnabled(kDirectSlot));
    CHECK(s.speaker.isEnabled(kDirectSlot));
}

TEST_CASE("the switches a claim carried are still there after a restart", "[slot-switches]") {
    const auto settings = makeSharedSettings();
    {
        Switches s(settings);
        s.motion.setEnabled(kSdlSlot, false);
        s.rumble.setEnabled(QString::fromStdString(kSdlSlot), false);
        carry(s, claim());
    }
    const Switches restarted(settings);
    CHECK_FALSE(restarted.motion.isEnabled(kDirectSlot));
    CHECK_FALSE(restarted.rumbleOn(kDirectSlot));
}
