// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The support report's second cut (docs/diagnostics-bundle.md, "Second cut"):
// the report's category, subject and body, the decisions about each host's
// snapshot and how they are gathered, the session's wire counts, each pad's
// output lanes and voices, the PC's power and thermal facts as sysfs gives
// them, and the Bluetooth link a HID node rides. The decisions are pure; the
// sysfs cases read a folder each test lays out for itself.

#include "composer/HostSnapshotCollector.h"
#include "core/diagnostics/DeviceFacts.h"
#include "core/diagnostics/HostSnapshot.h"
#include "core/diagnostics/Json.h"
#include "core/diagnostics/PadLanes.h"
#include "core/diagnostics/ReportForm.h"
#include "core/diagnostics/WireCounters.h"
#include "core/input/HidTransport.h"
#include "core/model/Protocol.h"
#include "source/system/PowerStatus.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using dish::composer::HostSnapshotCollector;
using dish::composer::HostSnapshotGateway;
using dish::composer::HostSnapshots;
using dish::composer::SnapshotFetchReply;
using dish::diagnostics::Json;
using dish::diagnostics::OutputLane;
using dish::diagnostics::PadOutputCounters;
using dish::diagnostics::PowerSupplyReading;
using dish::diagnostics::ReceivedKind;
using dish::diagnostics::ReportCategory;
using dish::diagnostics::SentKind;
using dish::diagnostics::SnapshotError;
using dish::diagnostics::SnapshotReply;
using dish::diagnostics::ThermalTrip;
using dish::diagnostics::ThermalZoneReading;
using dish::diagnostics::VoiceState;
using dish::diagnostics::WireCounters;

namespace {

// Answers each fetch only when the test says, so the collector's waiting is observable.
class HeldGateway : public HostSnapshotGateway {
  public:
    void fetch(const std::string& connectionKey, Reply reply) override {
        asked.push_back(connectionKey);
        pending.emplace(connectionKey, std::move(reply));
    }

    void answer(const std::string& connectionKey, SnapshotFetchReply reply) {
        const auto it = pending.find(connectionKey);
        REQUIRE(it != pending.end());
        Reply waiting = std::move(it->second);
        pending.erase(it);
        waiting(std::move(reply));
    }

    std::vector<std::string> asked;
    std::map<std::string, Reply> pending;
};

struct CollectedSnapshots {
    int calls = 0;
    HostSnapshots last;

    void take(HostSnapshots snapshots) {
        ++calls;
        last = std::move(snapshots);
    }
};

Json connectionSnapshot() {
    Json snapshot = Json::object();
    snapshot.set("format", "satellite-connection-diagnostics.v1");
    return snapshot;
}

} // namespace

// ── The report ──────────────────────────────────────────────────────────────

TEST_CASE("report: each category has the contract's token and reads back from it",
          "[diagnostics][report]") {
    CHECK(reportCategoryToken(ReportCategory::NotRecognised) == "notRecognised");
    CHECK(reportCategoryToken(ReportCategory::NoFeedback) == "noFeedback");
    CHECK(reportCategoryToken(ReportCategory::Connection) == "connection");
    CHECK(reportCategoryToken(ReportCategory::Audio) == "audio");
    CHECK(reportCategoryToken(ReportCategory::Other) == "other");
    CHECK(reportCategoryToken(ReportCategory::Question) == "question");
    for (const auto& row : dish::diagnostics::kReportCategories) {
        CHECK(dish::diagnostics::reportCategoryFromToken(row.token) ==
              std::optional<ReportCategory>(row.category));
    }
    CHECK_FALSE(dish::diagnostics::reportCategoryFromToken("bug").has_value());
}

TEST_CASE("report: the subject names the category in the person's words, then the app and PC",
          "[diagnostics][report]") {
    CHECK(dish::diagnostics::reportSubject("Keine Vibration", "2.8.0", "AYN Thor") ==
          "Dish support: Keine Vibration \xC2\xB7 Dish for Linux 2.8.0 on AYN Thor");
}

TEST_CASE("report: the person's words go first in the body, the summary after",
          "[diagnostics][report]") {
    SECTION("their text, a blank line, the summary") {
        CHECK(dish::diagnostics::reportMailBody("  It never rumbles.\n", "Dish 2.8.0") ==
              "It never rumbles.\n\nDish 2.8.0");
    }
    SECTION("nothing written leaves the summary alone") {
        CHECK(dish::diagnostics::reportMailBody(" \n\t", "Dish 2.8.0") == "Dish 2.8.0");
    }
}

TEST_CASE("report: the controller is its record, else its product string, else nothing",
          "[diagnostics][report]") {
    CHECK(dish::diagnostics::reportControllerOf(std::string("sony-dualsense"), "Wireless") ==
          std::optional<std::string>("sony-dualsense"));
    CHECK(dish::diagnostics::reportControllerOf(std::nullopt, "Wireless Controller") ==
          std::optional<std::string>("Wireless Controller"));
    CHECK_FALSE(dish::diagnostics::reportControllerOf(std::nullopt, "").has_value());
}

// ── Host snapshots ──────────────────────────────────────────────────────────

TEST_CASE("host snapshot: what each answer to the diagnostics route means",
          "[diagnostics][snapshot]") {
    using dish::diagnostics::snapshotErrorOf;
    CHECK(snapshotErrorOf(SnapshotReply{200, false, true}) == SnapshotError::None);
    // Satellite 2.3.0 answers 404 for a connection it does not know or that is another device's;
    // an older Satellite is told by its version and never asked.
    CHECK(snapshotErrorOf(SnapshotReply{404, false, false}) == SnapshotError::UnknownConnection);
    CHECK(snapshotErrorOf(SnapshotReply{401, false, false}) == SnapshotError::Unauthorized);
    CHECK(snapshotErrorOf(SnapshotReply{403, false, false}) == SnapshotError::Unauthorized);
    CHECK(snapshotErrorOf(SnapshotReply{0, true, false}) == SnapshotError::Timeout);
    CHECK(snapshotErrorOf(SnapshotReply{0, false, false}) == SnapshotError::Offline);
    SECTION("an answer that is not a snapshot is a bad reply, not an unreachable host") {
        CHECK(snapshotErrorOf(SnapshotReply{200, false, false}) == SnapshotError::BadReply);
        CHECK(snapshotErrorOf(SnapshotReply{503, false, false}) == SnapshotError::BadReply);
        CHECK(snapshotErrorOf(SnapshotReply{500, false, true}) == SnapshotError::BadReply);
    }
}

TEST_CASE("host snapshot: the error tokens are the contract's, and no error writes none",
          "[diagnostics][snapshot]") {
    using dish::diagnostics::snapshotErrorToken;
    CHECK_FALSE(snapshotErrorToken(SnapshotError::None).has_value());
    CHECK(snapshotErrorToken(SnapshotError::Offline) == std::optional<std::string_view>("offline"));
    CHECK(snapshotErrorToken(SnapshotError::Unauthorized) ==
          std::optional<std::string_view>("unauthorized"));
    CHECK(snapshotErrorToken(SnapshotError::OlderSatellite) ==
          std::optional<std::string_view>("olderSatellite"));
    CHECK(snapshotErrorToken(SnapshotError::Timeout) == std::optional<std::string_view>("timeout"));
    CHECK(snapshotErrorToken(SnapshotError::UnknownConnection) ==
          std::optional<std::string_view>("unknownConnection"));
    CHECK(snapshotErrorToken(SnapshotError::BadReply) ==
          std::optional<std::string_view>("badReply"));
    CHECK(dish::diagnostics::kSnapshotTimeoutMs == 2000);
}

TEST_CASE("host snapshot: a Satellite below 2.3.0 has no route, told by the version it reported",
          "[diagnostics][snapshot]") {
    using dish::diagnostics::isOlderSatellite;
    CHECK(isOlderSatellite("2.2.9"));
    CHECK(isOlderSatellite("2.2"));
    CHECK(isOlderSatellite("1.9.0"));
    CHECK_FALSE(isOlderSatellite("2.3.0"));
    CHECK_FALSE(isOlderSatellite("2.3.0-rc.1"));
    CHECK_FALSE(isOlderSatellite("2.10.0"));
    CHECK_FALSE(isOlderSatellite("3.0.0"));
    SECTION("a version it cannot read is not taken for an old one, so the route is asked") {
        CHECK_FALSE(isOlderSatellite(""));
        CHECK_FALSE(isOlderSatellite("dev"));
    }
}

TEST_CASE("host snapshot collector: every live link is asked once, and the answers wait for all",
          "[diagnostics][snapshot]") {
    HeldGateway gateway;
    CollectedSnapshots collected;
    HostSnapshotCollector collector(gateway);
    collector.collect({"sat-a", "sat-b"}, [&collected](HostSnapshots snapshots) {
        collected.take(std::move(snapshots));
    });
    CHECK(gateway.asked == std::vector<std::string>{"sat-a", "sat-b"});

    gateway.answer("sat-a", SnapshotFetchReply{200, false, connectionSnapshot()});
    CHECK(collected.calls == 0);
    gateway.answer("sat-b", SnapshotFetchReply{401, false, std::nullopt});
    REQUIRE(collected.calls == 1);

    const auto& first = collected.last.at("sat-a");
    CHECK(first.error == SnapshotError::None);
    REQUIRE(first.snapshot.has_value());
    CHECK(first.snapshot->find("format")->stringValue() == "satellite-connection-diagnostics.v1");
    const auto& second = collected.last.at("sat-b");
    CHECK(second.error == SnapshotError::Unauthorized);
    CHECK_FALSE(second.snapshot.has_value());
}

TEST_CASE("host snapshot collector: with no live link the answer is at once and empty",
          "[diagnostics][snapshot]") {
    HeldGateway gateway;
    CollectedSnapshots collected;
    HostSnapshotCollector collector(gateway);
    collector.collect(
        {}, [&collected](HostSnapshots snapshots) { collected.take(std::move(snapshots)); });
    CHECK(collected.calls == 1);
    CHECK(collected.last.empty());
    CHECK(gateway.asked.empty());
}

TEST_CASE("host snapshot collector: a body that is no snapshot is dropped, never embedded",
          "[diagnostics][snapshot]") {
    HeldGateway gateway;
    CollectedSnapshots collected;
    HostSnapshotCollector collector(gateway);
    collector.collect(
        {"sat-a"}, [&collected](HostSnapshots snapshots) { collected.take(std::move(snapshots)); });
    gateway.answer("sat-a", SnapshotFetchReply{200, false, std::nullopt});
    REQUIRE(collected.calls == 1);
    CHECK(collected.last.at("sat-a").error == SnapshotError::BadReply);
    CHECK_FALSE(collected.last.at("sat-a").snapshot.has_value());
}

TEST_CASE("host snapshot collector: an answer after the collector is gone reaches nobody",
          "[diagnostics][snapshot]") {
    HeldGateway gateway;
    CollectedSnapshots collected;
    {
        HostSnapshotCollector collector(gateway);
        collector.collect({"sat-a"}, [&collected](HostSnapshots snapshots) {
            collected.take(std::move(snapshots));
        });
    }
    gateway.answer("sat-a", SnapshotFetchReply{0, true, std::nullopt});
    CHECK(collected.calls == 0);
}

// ── The session's own view ──────────────────────────────────────────────────

TEST_CASE("wire counters: each message kind counts under its own name", "[diagnostics][wire]") {
    WireCounters counters;
    counters.countSent(dish::proto::kMsgInput);
    counters.countSent(dish::proto::kMsgInput);
    counters.countSent(dish::proto::kMsgTouchpad);
    counters.countSent(dish::proto::kMsgMicAudio);
    counters.countReceived(dish::proto::kMsgRumble);
    counters.countReceived(dish::proto::kMsgHapticAudio);
    counters.countReceived(dish::proto::kMsgSessionClose);
    const auto counts = counters.counts();
    CHECK(counts.sent[static_cast<std::size_t>(SentKind::Input)] == 2);
    CHECK(counts.sent[static_cast<std::size_t>(SentKind::Pointer)] == 1);
    CHECK(counts.sent[static_cast<std::size_t>(SentKind::Mic)] == 1);
    CHECK(counts.sent[static_cast<std::size_t>(SentKind::Motion)] == 0);
    CHECK(counts.received[static_cast<std::size_t>(ReceivedKind::Rumble)] == 1);
    CHECK(counts.received[static_cast<std::size_t>(ReceivedKind::Haptic)] == 1);
    CHECK(counts.received[static_cast<std::size_t>(ReceivedKind::SessionClose)] == 1);
}

TEST_CASE("wire counters: the heartbeat and kinds no table names are not counted",
          "[diagnostics][wire]") {
    WireCounters counters;
    counters.countSent(dish::proto::kMsgHeartbeat);
    counters.countReceived(dish::proto::kMsgHeartbeatAck);
    counters.countReceived(0x7FFF);
    CHECK(counters.counts() == dish::diagnostics::WireCounts{});
}

TEST_CASE("wire counters: the names are the contract's, in its order", "[diagnostics][wire]") {
    std::vector<std::string> sent;
    for (const auto& row : dish::diagnostics::kSentKinds) { sent.emplace_back(row.token); }
    CHECK(sent == std::vector<std::string>{"input", "motion", "battery", "pointer", "mic"});
    std::vector<std::string> received;
    for (const auto& row : dish::diagnostics::kReceivedKinds) { received.emplace_back(row.token); }
    CHECK(received == std::vector<std::string>{"rumble", "lightbar", "triggerEffects", "playerLeds",
                                               "micLed", "speaker", "haptic", "sessionClose"});
}

// ── Each pad's lanes ────────────────────────────────────────────────────────

TEST_CASE("pad lanes: each lane keeps its count, its last value and when", "[diagnostics][lanes]") {
    PadOutputCounters counters;
    counters.record(OutputLane::Rumble, dish::diagnostics::packRumble(32768, 0), 1000);
    counters.record(OutputLane::Rumble, dish::diagnostics::packRumble(0, 12), 2000);
    counters.record(OutputLane::Lightbar, dish::diagnostics::packRgb(0, 0, 255), 3000);
    const auto rumble = counters.sample(OutputLane::Rumble);
    CHECK(rumble.count == 2);
    CHECK(rumble.lastAtMs == 2000);
    CHECK(dish::diagnostics::rumbleStrongOf(rumble.last) == 0);
    CHECK(dish::diagnostics::rumbleWeakOf(rumble.last) == 12);
    const auto lightbar = counters.sample(OutputLane::Lightbar);
    CHECK(lightbar.count == 1);
    CHECK(dish::diagnostics::rgbRedOf(lightbar.last) == 0);
    CHECK(dish::diagnostics::rgbBlueOf(lightbar.last) == 255);
    CHECK(counters.sample(OutputLane::MicLed).count == 0);
    CHECK(counters.sample(OutputLane::MicLed).lastAtMs == 0);
}

TEST_CASE("pad lanes: the packed values survive their extremes", "[diagnostics][lanes]") {
    constexpr std::uint16_t kMax = std::numeric_limits<std::uint16_t>::max();
    const std::uint64_t packed = dish::diagnostics::packRumble(kMax, 1);
    CHECK(dish::diagnostics::rumbleStrongOf(packed) == kMax);
    CHECK(dish::diagnostics::rumbleWeakOf(packed) == 1);
    const std::uint64_t rgb = dish::diagnostics::packRgb(255, 128, 1);
    CHECK(dish::diagnostics::rgbRedOf(rgb) == 255);
    CHECK(dish::diagnostics::rgbGreenOf(rgb) == 128);
    CHECK(dish::diagnostics::rgbBlueOf(rgb) == 1);
}

TEST_CASE("pad lanes: where a lane's output goes, in the contract's words",
          "[diagnostics][lanes]") {
    using dish::reducer::FeedbackTarget;
    CHECK(dish::diagnostics::feedbackTargetToken(FeedbackTarget::DirectUsb) == "directUsb");
    CHECK(dish::diagnostics::feedbackTargetToken(FeedbackTarget::Standard) == "framework");
    CHECK(dish::diagnostics::feedbackTargetToken(FeedbackTarget::None) == "none");
}

TEST_CASE("pad lanes: a voice is refused, open or closed, in that order of precedence",
          "[diagnostics][lanes]") {
    using dish::diagnostics::voiceStateOf;
    CHECK(voiceStateOf(false, true) == VoiceState::Refused);
    CHECK(voiceStateOf(true, true) == VoiceState::Refused);
    CHECK(voiceStateOf(true, false) == VoiceState::Open);
    CHECK(voiceStateOf(false, false) == VoiceState::Closed);
    CHECK(dish::diagnostics::voiceStateToken(VoiceState::Open) == "open");
    CHECK(dish::diagnostics::voiceStateToken(VoiceState::Closed) == "closed");
    CHECK(dish::diagnostics::voiceStateToken(VoiceState::Refused) == "refused");
}

// ── Device facts ────────────────────────────────────────────────────────────

namespace {

PowerSupplyReading supply(std::string type, std::string scope, std::string status,
                          std::optional<int> capacity) {
    return PowerSupplyReading{std::move(type), std::move(scope), std::move(status), capacity};
}

// One file under `dir`, its folders made on the way.
void writeFile(const QString& dir, const QString& name, const QByteArray& body) {
    const QString path = dir + QLatin1Char('/') + name;
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(body);
}

} // namespace

TEST_CASE("device facts: a laptop on battery with the low-power profile", "[diagnostics][device]") {
    const auto facts = dish::diagnostics::powerFactsOf(
        {supply("Mains", "", "", std::nullopt), supply("Battery", "System", "Discharging", 18)},
        std::optional<std::string>("low-power"));
    REQUIRE(facts.battery.has_value());
    CHECK(facts.battery->level == std::optional<int>(18));
    CHECK_FALSE(facts.battery->charging);
    CHECK(facts.powerSave == std::optional<bool>(true));
}

TEST_CASE("device facts: charging is the battery's own status word", "[diagnostics][device]") {
    const auto facts = dish::diagnostics::powerFactsOf({supply("Battery", "", "Charging", 64)},
                                                       std::optional<std::string>("balanced"));
    REQUIRE(facts.battery.has_value());
    CHECK(facts.battery->charging);
    CHECK(facts.powerSave == std::optional<bool>(false));
    const auto full =
        dish::diagnostics::powerFactsOf({supply("Battery", "", "Full", 100)}, std::nullopt);
    CHECK_FALSE(full.battery->charging);
}

TEST_CASE("device facts: a pad's own battery is not the PC's, and unknowns stay unknown",
          "[diagnostics][device]") {
    // A DualSense over Bluetooth adds ps-controller-battery-<addr> with scope Device.
    const auto desktop = dish::diagnostics::powerFactsOf(
        {supply("Battery", "Device", "Discharging", 70), supply("USB", "", "", std::nullopt)},
        std::nullopt);
    CHECK_FALSE(desktop.battery.has_value());
    const auto unknownLevel = dish::diagnostics::powerFactsOf(
        {supply("Battery", "", "Unknown", std::nullopt), supply("Battery", "", "Charging", 50)},
        std::nullopt);
    REQUIRE(unknownLevel.battery.has_value());
    CHECK_FALSE(unknownLevel.battery->level.has_value());
    CHECK_FALSE(unknownLevel.battery->charging);
    const auto outOfRange =
        dish::diagnostics::powerFactsOf({supply("Battery", "", "", 140)}, std::nullopt);
    CHECK_FALSE(outOfRange.battery->level.has_value());
}

TEST_CASE("device facts: no power profile and no supply says nothing", "[diagnostics][device]") {
    const auto facts = dish::diagnostics::powerFactsOf({}, std::nullopt);
    CHECK_FALSE(facts.battery.has_value());
    CHECK_FALSE(facts.powerSave.has_value());
}

TEST_CASE("device facts: the thermal state is the worst trip point any zone has crossed",
          "[diagnostics][device]") {
    using dish::diagnostics::ThermalState;
    using dish::diagnostics::thermalStateOf;
    const ThermalTrip fan{"active", 50000};
    const ThermalTrip throttle{"passive", 90000};
    const ThermalTrip shutdown{"critical", 105000};
    CHECK(thermalStateOf({ThermalZoneReading{45000, {fan, throttle, shutdown}}}) ==
          std::optional<ThermalState>(ThermalState::Nominal));
    CHECK(thermalStateOf({ThermalZoneReading{55000, {fan, throttle, shutdown}}}) ==
          std::optional<ThermalState>(ThermalState::Fair));
    CHECK(thermalStateOf({ThermalZoneReading{91000, {fan, throttle, shutdown}}}) ==
          std::optional<ThermalState>(ThermalState::Serious));
    CHECK(thermalStateOf({ThermalZoneReading{40000, {fan}},
                          ThermalZoneReading{106000, {ThermalTrip{"hot", 100000}}}}) ==
          std::optional<ThermalState>(ThermalState::Critical));
}

TEST_CASE("device facts: a zone with no usable trip point or no temperature judges nothing",
          "[diagnostics][device]") {
    using dish::diagnostics::thermalStateOf;
    CHECK_FALSE(thermalStateOf({}).has_value());
    CHECK_FALSE(thermalStateOf({ThermalZoneReading{60000, {}}}).has_value());
    CHECK_FALSE(
        thermalStateOf({ThermalZoneReading{60000, {ThermalTrip{"passive", 0}}}}).has_value());
    CHECK_FALSE(thermalStateOf({ThermalZoneReading{std::nullopt, {ThermalTrip{"passive", 1}}}})
                    .has_value());
    CHECK_FALSE(
        thermalStateOf({ThermalZoneReading{60000, {ThermalTrip{"user", 1000}}}}).has_value());
}

TEST_CASE("device facts: the thermal and power tokens are the contract's",
          "[diagnostics][device]") {
    using dish::diagnostics::ThermalState;
    using dish::diagnostics::thermalStateToken;
    CHECK(thermalStateToken(ThermalState::Nominal) == "nominal");
    CHECK(thermalStateToken(ThermalState::Fair) == "fair");
    CHECK(thermalStateToken(ThermalState::Serious) == "serious");
    CHECK(thermalStateToken(ThermalState::Critical) == "critical");
}

TEST_CASE("device facts: sysfs as it lies on a handheld", "[diagnostics][device][sysfs]") {
    QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString supplies = root.filePath(QStringLiteral("power_supply"));
    writeFile(supplies, QStringLiteral("ADP1/type"), "Mains\n");
    writeFile(supplies, QStringLiteral("BAT0/type"), "Battery\n");
    writeFile(supplies, QStringLiteral("BAT0/status"), "Charging\n");
    writeFile(supplies, QStringLiteral("BAT0/capacity"), "77\n");
    writeFile(supplies, QStringLiteral("ps-controller-battery-aa/type"), "Battery\n");
    writeFile(supplies, QStringLiteral("ps-controller-battery-aa/scope"), "Device\n");
    const QString profile = root.filePath(QStringLiteral("platform_profile"));
    writeFile(root.path(), QStringLiteral("platform_profile"), "low-power\n");
    const auto power = dish::source::readPowerFacts(supplies, profile);
    REQUIRE(power.battery.has_value());
    CHECK(power.battery->level == std::optional<int>(77));
    CHECK(power.battery->charging);
    CHECK(power.powerSave == std::optional<bool>(true));

    const QString thermal = root.filePath(QStringLiteral("thermal"));
    writeFile(thermal, QStringLiteral("thermal_zone0/temp"), "92000\n");
    writeFile(thermal, QStringLiteral("thermal_zone0/trip_point_0_type"), "passive\n");
    writeFile(thermal, QStringLiteral("thermal_zone0/trip_point_0_temp"), "90000\n");
    writeFile(thermal, QStringLiteral("thermal_zone0/trip_point_1_type"), "critical\n");
    writeFile(thermal, QStringLiteral("thermal_zone0/trip_point_1_temp"), "105000\n");
    writeFile(thermal, QStringLiteral("cooling_device0/type"), "Processor\n");
    CHECK(dish::source::readThermalState(thermal) ==
          std::optional<dish::diagnostics::ThermalState>(dish::diagnostics::ThermalState::Serious));
}

TEST_CASE("device facts: a PC whose sysfs has none of the files says null",
          "[diagnostics][device][sysfs]") {
    QTemporaryDir root;
    REQUIRE(root.isValid());
    const auto power = dish::source::readPowerFacts(root.filePath(QStringLiteral("none")),
                                                    root.filePath(QStringLiteral("none")));
    CHECK_FALSE(power.battery.has_value());
    CHECK_FALSE(power.powerSave.has_value());
    CHECK_FALSE(dish::source::readThermalState(root.filePath(QStringLiteral("none"))).has_value());
}

// ── Bluetooth ───────────────────────────────────────────────────────────────

TEST_CASE("bluetooth: the link is who made the HID device, never more of the path",
          "[diagnostics][bluetooth]") {
    using dish::input::BluetoothLink;
    using dish::input::bluetoothLinkOfHidDevice;
    CHECK(bluetoothLinkOfHidDevice(
              true, "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-10/1-10:1.0/bluetooth/hci0/"
                    "hci0:256/0005:054C:0CE6.0007") == BluetoothLink::Classic);
    CHECK(bluetoothLinkOfHidDevice(true, "/sys/devices/virtual/misc/uhid/0005:045E:0B13.0009") ==
          BluetoothLink::LowEnergy);
    CHECK(bluetoothLinkOfHidDevice(
              false, "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.3/0003:054C:0CE6.0003") ==
          BluetoothLink::None);
    // A uhid device that is not on the Bluetooth bus (a userspace driver's) is no BLE pad.
    CHECK(bluetoothLinkOfHidDevice(false, "/sys/devices/virtual/misc/uhid/0003:1234:5678.0001") ==
          BluetoothLink::None);
}

TEST_CASE("bluetooth: a hidraw and an evdev node resolve through sysfs's links",
          "[diagnostics][bluetooth][sysfs]") {
    using dish::input::BluetoothLink;
    using dish::input::bluetoothLinkOfHidPath;
    QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString sys = root.path();
    const QString ble = QStringLiteral("devices/virtual/misc/uhid/0005:045E:0B13.0009");
    writeFile(sys, ble + QStringLiteral("/uevent"), "HID_ID=0005:0000045E:00000B13\n");
    REQUIRE(QDir().mkpath(sys + QStringLiteral("/class/hidraw/hidraw3")));
    REQUIRE(QFile::link(sys + QLatin1Char('/') + ble,
                        sys + QStringLiteral("/class/hidraw/hidraw3/device")));
    const QString classic = QStringLiteral("devices/hci0/hci0:256/0005:054C:0CE6.0007");
    writeFile(sys, classic + QStringLiteral("/uevent"), "HID_ID=0005:0000054C:00000CE6\n");
    REQUIRE(QDir().mkpath(sys + QStringLiteral("/") + classic + QStringLiteral("/input/input9")));
    // input9's own `device` is the HID device it hangs off, as in sysfs.
    REQUIRE(QFile::link(sys + QLatin1Char('/') + classic,
                        sys + QLatin1Char('/') + classic + QStringLiteral("/input/input9/device")));
    REQUIRE(QDir().mkpath(sys + QStringLiteral("/class/input/event9")));
    REQUIRE(QFile::link(sys + QLatin1Char('/') + classic + QStringLiteral("/input/input9"),
                        sys + QStringLiteral("/class/input/event9/device")));
    const std::string sysRoot = sys.toStdString();
    CHECK(bluetoothLinkOfHidPath("/dev/hidraw3", sysRoot) == BluetoothLink::LowEnergy);
    CHECK(bluetoothLinkOfHidPath("/dev/input/event9", sysRoot) == BluetoothLink::Classic);
    CHECK(bluetoothLinkOfHidPath("/dev/hidraw4", sysRoot) == BluetoothLink::None);
    CHECK(bluetoothLinkOfHidPath("", sysRoot) == BluetoothLink::None);
    CHECK(bluetoothLinkOfHidPath("some-opaque-sdl-identifier", sysRoot) == BluetoothLink::None);
}

// ── The JSON writer's numbers ───────────────────────────────────────────────

TEST_CASE("json: a fraction is written to three places, whatever the C locale says",
          "[diagnostics][json]") {
    CHECK(Json(2.1).dump() == "2.1\n");
    CHECK(Json(4.8125).dump() == "4.813\n");
    CHECK(Json(3.0).dump() == "3\n");
    CHECK(Json(-0.25).dump() == "-0.25\n");
    CHECK(Json(0.0004).dump() == "0\n");
    CHECK(Json(std::numeric_limits<double>::quiet_NaN()).dump() == "null\n");
    CHECK(Json(std::numeric_limits<double>::infinity()).dump() == "null\n");
    CHECK(Json(2.5).doubleValue() == 2.5);
}

TEST_CASE("json: an object's keys read back in their order", "[diagnostics][json]") {
    Json object = Json::object();
    object.set("b", 1);
    object.set("a", 2);
    CHECK(object.keys() == std::vector<std::string>{"b", "a"});
}
