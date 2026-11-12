// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A Direct pad's feedback: producers post to the claim's worker and return at once, the worker
// writes on its own thread through the HID writer seam. The real writer is driven against a pipe,
// which takes a write the way a hidraw node does.

#include "source/usb/HidReportWriter.h"
#include "source/usb/OutputWorker.h"

#include "core/input/UsbReportParsers.h"
#include "core/thread/TrackedMutex.h"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using dish::input::usbparse::HidParser;
using dish::source::usb::FdReportWriter;
using dish::source::usb::HidReportWriter;
using dish::source::usb::OutputWorker;
using dish::source::usb::writeFeatureWithRetry;
using namespace std::chrono_literals;

namespace {

// A pad that takes `delay` to accept each report, recording what it got and the thread that
// wrote each feature report. It refuses the first `featureRefusals` feature reports and every one
// whose message id is in `refusedMessages`, the way a dongle under load refuses a write.
struct SlowPad {
    std::chrono::milliseconds delay{0};
    int featureRefusals = 0;
    std::vector<std::uint8_t> refusedMessages;
    mutable std::mutex mtx;
    std::vector<std::vector<std::uint8_t>> reports;
    std::vector<std::vector<std::uint8_t>> features;
    std::vector<std::thread::id> featureWriters;
    int featureCalls = 0;

    std::vector<std::vector<std::uint8_t>> got() const {
        const std::lock_guard<std::mutex> lock(mtx);
        return reports;
    }
    std::vector<std::vector<std::uint8_t>> gotFeatures() const {
        const std::lock_guard<std::mutex> lock(mtx);
        return features;
    }
    std::vector<std::thread::id> featureWriterIds() const {
        const std::lock_guard<std::mutex> lock(mtx);
        return featureWriters;
    }
    int featureCallCount() const {
        const std::lock_guard<std::mutex> lock(mtx);
        return featureCalls;
    }
};

class SlowWriter final : public HidReportWriter {
  public:
    explicit SlowWriter(SlowPad& pad) : pad_(pad) {}
    bool write(const std::uint8_t* data, std::size_t len) override {
        std::this_thread::sleep_for(pad_.delay);
        const std::lock_guard<std::mutex> lock(pad_.mtx);
        pad_.reports.emplace_back(data, data + len);
        return true;
    }
    bool writeFeature(const std::uint8_t* data, std::size_t len) override {
        const std::lock_guard<std::mutex> lock(pad_.mtx);
        ++pad_.featureCalls;
        if (pad_.featureRefusals > 0) {
            --pad_.featureRefusals;
            return false;
        }
        const bool messageRefused =
            len > 1 && std::find(pad_.refusedMessages.begin(), pad_.refusedMessages.end(),
                                 data[1]) != pad_.refusedMessages.end();
        if (messageRefused) { return false; }
        pad_.features.emplace_back(data, data + len);
        pad_.featureWriters.push_back(std::this_thread::get_id());
        return true;
    }

  private:
    SlowPad& pad_;
};

// The 2015 Steam Controller's config reports: report id 0 at 0, the message id at 1.
constexpr std::uint8_t kSteamClearMappings = 0x81;
constexpr std::uint8_t kSteamSetSettings = 0x87;
constexpr std::uint8_t kSteamDefaultMappings = 0x85;
constexpr std::uint8_t kSteamDefaultSettings = 0x8E;

std::vector<std::uint8_t> steamMessages(const std::vector<std::vector<std::uint8_t>>& features) {
    std::vector<std::uint8_t> out;
    for (const auto& feature : features) { out.push_back(feature.size() > 1 ? feature[1] : 0); }
    return out;
}

bool waitFor(const SlowPad& pad, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pad.got().size() >= count) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool waitForFeatures(const SlowPad& pad, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pad.gotFeatures().size() >= count) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

// The 2026 Steam Controller's haptic rumble report: id 0x80, the left (strong) speed at 4..5 and
// the right (weak) one at 7..8, little-endian.
bool isTritonRumbleStop(const std::vector<std::uint8_t>& report) {
    return report.size() == 10 && report[0] == 0x80 && report[4] == 0 && report[5] == 0 &&
           report[7] == 0 && report[8] == 0;
}

bool waitForRumbleStop(const SlowPad& pad) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto reports = pad.got();
        if (!reports.empty() && isTritonRumbleStop(reports.back())) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

// The DualSense rumble report carries the strong motor's high byte at 4.
constexpr std::size_t kStrongByte = 4;

// A pad's node as a pipe: the writer gets the write end, the test reads what reached the pad.
struct PipePad {
    std::array<int, 2> fds{-1, -1};

    PipePad() { REQUIRE(::pipe2(fds.data(), O_CLOEXEC) == 0); }
    ~PipePad() {
        for (const int fd : fds) {
            if (fd >= 0) { ::close(fd); }
        }
    }
    PipePad(const PipePad&) = delete;
    PipePad& operator=(const PipePad&) = delete;
    PipePad(PipePad&&) = delete;
    PipePad& operator=(PipePad&&) = delete;

    int node() const { return fds[1]; }
    int pad() const { return fds[0]; }
};

} // namespace

TEST_CASE("a slow pad never delays the thread posting its feedback", "[output-worker]") {
    SlowPad pad;
    pad.delay = 100ms;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::DualSense);
    auto slowest = std::chrono::steady_clock::duration::zero();
    for (int i = 0; i < 50; ++i) {
        const auto started = std::chrono::steady_clock::now();
        worker.postRumble(static_cast<std::uint16_t>(i << 8), 0);
        slowest = std::max(slowest, std::chrono::steady_clock::now() - started);
    }
    CHECK(slowest < 20ms);
    CHECK(waitFor(pad, 1));
    worker.stop();
}

TEST_CASE("the newest feedback wins over a stale one", "[output-worker]") {
    SlowPad pad;
    pad.delay = 150ms;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::DualSense);
    worker.postRumble(0x1000, 0);
    // The first write is under way now; these two land while the pad is busy.
    std::this_thread::sleep_for(20ms);
    worker.postRumble(0x2000, 0);
    worker.postRumble(0x3000, 0);
    REQUIRE(waitFor(pad, 2));
    std::this_thread::sleep_for(400ms);
    const auto reports = pad.got();
    REQUIRE(reports.size() == 2U);
    CHECK(reports[0][kStrongByte] == 0x10);
    CHECK(reports[1][kStrongByte] == 0x30);
    worker.stop();
}

TEST_CASE("a stopped worker writes nothing more", "[output-worker]") {
    SlowPad pad;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::DualSense);
    worker.stop();
    worker.postLightbar(1, 2, 3);
    std::this_thread::sleep_for(50ms);
    CHECK(pad.got().empty());
    worker.stop();
}

TEST_CASE("a 2026 Steam Controller's worker sends its quiet settings at once",
          "[output-worker][triton]") {
    // The claim sends none itself; the worker owns them, at once and every 3 s.
    SlowPad pad;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamTriton);
    REQUIRE(waitForFeatures(pad, 2));
    const auto features = pad.gotFeatures();
    REQUIRE(features[0].size() == 64U);
    CHECK(features[0][0] == 0x01); // feature report 1
    CHECK(features[0][3] == 9);    // SETTING_LIZARD_MODE
    CHECK(features[0][4] == 0);    // LIZARD_MODE_OFF
    REQUIRE(features[1].size() == 64U);
    CHECK(features[1][3] == 48);   // SETTING_IMU_MODE
    CHECK(features[1][4] == 0x18); // raw accel | raw gyro
    CHECK(pad.got().empty());
    worker.stop();
}

TEST_CASE("a running 2026 rumble is sent again until a stop", "[output-worker][triton]") {
    // The pad's haptics stop on their own about 50 ms after a write, so the
    // worker repeats a running rumble every 40 ms and a stop once.
    SlowPad pad;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamTriton);
    worker.postRumble(0x8000, 0x4000);
    REQUIRE(waitFor(pad, 4));
    for (const auto& report : pad.got()) {
        REQUIRE(report.size() == 10U);
        CHECK(report[0] == 0x80);
        CHECK(report[5] == 0x80); // strong, high byte
        CHECK(report[8] == 0x40); // weak, high byte
    }
    worker.postRumble(0, 0);
    REQUIRE(waitForRumbleStop(pad));
    const std::size_t afterStop = pad.got().size();
    std::this_thread::sleep_for(200ms);
    CHECK(pad.got().size() == afterStop);
    worker.stop();
}

TEST_CASE("a 2015 Steam Controller's worker quiets a pad that is there, off the posting thread",
          "[output-worker][steam]") {
    SlowPad pad;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamController);
    const auto started = std::chrono::steady_clock::now();
    worker.postPadLink(true);
    CHECK(std::chrono::steady_clock::now() - started < 20ms);
    REQUIRE(waitForFeatures(pad, 2));
    const auto features = pad.gotFeatures();
    REQUIRE(features[0].size() == 65U);
    CHECK(features[0][0] == 0x00);
    CHECK(steamMessages(features) ==
          std::vector<std::uint8_t>{kSteamClearMappings, kSteamSetSettings});
    for (const auto& writer : pad.featureWriterIds()) {
        CHECK(writer != std::this_thread::get_id());
    }
    CHECK(pad.got().empty());
    worker.postPadLink(false);
    worker.stop();
}

TEST_CASE("a worker handed back takes the 2015 pad's settings off it", "[output-worker][steam]") {
    SlowPad pad;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamController);
    worker.postPadLink(true);
    REQUIRE(waitForFeatures(pad, 2));
    worker.stop();
    CHECK(steamMessages(pad.gotFeatures()) ==
          std::vector<std::uint8_t>{kSteamClearMappings, kSteamSetSettings, kSteamDefaultMappings,
                                    kSteamDefaultSettings, kSteamSetSettings});
}

TEST_CASE("a controller that left holds nothing to take back", "[output-worker][steam]") {
    SlowPad pad;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamController);
    worker.postPadLink(true);
    REQUIRE(waitForFeatures(pad, 2));
    worker.postPadLink(false);
    worker.stop();
    CHECK(pad.gotFeatures().size() == 2U);
}

TEST_CASE("a slot whose controller never came is written nothing", "[output-worker][steam]") {
    SlowPad pad;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamController);
    std::this_thread::sleep_for(50ms);
    worker.stop();
    CHECK(pad.featureCallCount() == 0);
}

TEST_CASE("a quiet the dongle refuses is offered again", "[output-worker][steam]") {
    SlowPad pad;
    pad.featureRefusals = 2;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamController);
    worker.postPadLink(true);
    REQUIRE(waitForFeatures(pad, 2));
    CHECK(pad.featureCallCount() == 4);
    CHECK(steamMessages(pad.gotFeatures()) ==
          std::vector<std::uint8_t>{kSteamClearMappings, kSteamSetSettings});
    worker.postPadLink(false);
    worker.stop();
}

TEST_CASE("a quiet the pad never takes is undone, and the pad then holds nothing",
          "[output-worker][steam]") {
    // The first report is offered 25 times, 20 ms apart, then the restore sequence goes; the
    // hand-back has nothing left to do.
    SlowPad pad;
    pad.refusedMessages = {kSteamClearMappings};
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamController);
    worker.postPadLink(true);
    REQUIRE(waitForFeatures(pad, 3));
    CHECK(steamMessages(pad.gotFeatures()) == std::vector<std::uint8_t>{kSteamDefaultMappings,
                                                                        kSteamDefaultSettings,
                                                                        kSteamSetSettings});
    CHECK(pad.featureCallCount() == 28);
    worker.stop();
    CHECK(pad.featureCallCount() == 28);
}

TEST_CASE("a 2026 slot's settings are taken back only from a controller that is there",
          "[output-worker][triton]") {
    SECTION("no controller: the settings went to the slot, nothing comes back") {
        SlowPad pad;
        OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamTriton);
        REQUIRE(waitForFeatures(pad, 2));
        worker.stop();
        CHECK(pad.gotFeatures().size() == 2U);
    }
    SECTION("a controller there: the IMU off, then lizard mode back on") {
        SlowPad pad;
        OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::SteamTriton);
        REQUIRE(waitForFeatures(pad, 2));
        worker.postPadLink(true);
        worker.stop();
        const auto features = pad.gotFeatures();
        REQUIRE(features.size() == 4U);
        CHECK(features[2][3] == 48); // SETTING_IMU_MODE
        CHECK(features[2][4] == 0);  // off
        CHECK(features[3][3] == 9);  // SETTING_LIZARD_MODE
        CHECK(features[3][4] == 1);  // on
    }
}

TEST_CASE("what is posted before the worker starts is taken when it does", "[output-worker]") {
    SlowPad pad;
    OutputWorker worker(HidParser::SteamController);
    worker.postPadLink(true);
    std::this_thread::sleep_for(20ms);
    CHECK(pad.featureCallCount() == 0);
    worker.start(std::make_unique<SlowWriter>(pad));
    REQUIRE(waitForFeatures(pad, 2));
    worker.postPadLink(false);
    worker.stop();
}

TEST_CASE("a refused feature write is offered again, and a taken one ends the offers",
          "[output-worker][hid]") {
    SlowPad pad;
    pad.featureRefusals = 2;
    SlowWriter writer(pad);
    const std::array<std::uint8_t, 3> report{0x00, kSteamClearMappings, 0x00};
    CHECK(writeFeatureWithRetry(writer, report.data(), report.size(), 5));
    CHECK(pad.featureCallCount() == 3);
    pad.featureRefusals = 10;
    CHECK_FALSE(writeFeatureWithRetry(writer, report.data(), report.size(), 3));
    CHECK(pad.featureCallCount() == 6);
}

TEST_CASE("a family with nothing on a clock writes only what it is asked", "[output-worker]") {
    SlowPad pad;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::DualSense);
    worker.postRumble(0x8000, 0);
    REQUIRE(waitFor(pad, 1));
    std::this_thread::sleep_for(150ms);
    CHECK(pad.got().size() == 1U);
    CHECK(pad.gotFeatures().empty());
    worker.stop();
}

TEST_CASE("a family without the surface writes nothing and counts the refusal", "[output-worker]") {
    SlowPad pad;
    OutputWorker worker(std::make_unique<SlowWriter>(pad), HidParser::DualShock4);
    worker.postPlayerLeds(0x01);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (worker.failedWrites() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(worker.failedWrites() == 1U);
    CHECK(pad.got().empty());
}

TEST_CASE("the fd writer hands the node the report exactly as built", "[output-worker][hid]") {
    PipePad pipe;
    dish::thread::TrackedMutex writeMtx{"hid.write"};
    FdReportWriter writer(pipe.node(), &writeMtx);
    const std::array<std::uint8_t, 3> report{0x05, 0x11, 0x22};
    CHECK(writer.write(report.data(), report.size()));
    CHECK_FALSE(writer.write(report.data(), 0));
    CHECK_FALSE(writer.write(nullptr, report.size()));
    std::array<std::uint8_t, 8> got{};
    const ssize_t read = ::read(pipe.pad(), got.data(), got.size());
    REQUIRE(read == 3);
    CHECK(got == std::array<std::uint8_t, 8>{0x05, 0x11, 0x22, 0, 0, 0, 0, 0});
}

TEST_CASE("the fd writer refuses a node that is not open", "[output-worker][hid]") {
    dish::thread::TrackedMutex writeMtx{"hid.write"};
    FdReportWriter writer(-1, &writeMtx);
    const std::array<std::uint8_t, 3> report{0x05, 0x11, 0x22};
    CHECK_FALSE(writer.write(report.data(), report.size()));
}

TEST_CASE("the fd writer reports a feature write the node refuses", "[output-worker][hid]") {
    // A pipe takes no HIDIOCSFEATURE, the way a node that is no hidraw would not.
    PipePad pipe;
    dish::thread::TrackedMutex writeMtx{"hid.write"};
    FdReportWriter writer(pipe.node(), &writeMtx);
    const std::array<std::uint8_t, 3> report{0x01, 0x87, 0x03};
    CHECK_FALSE(writer.writeFeature(report.data(), report.size()));
    CHECK_FALSE(writer.writeFeature(report.data(), 0));
    CHECK_FALSE(writer.writeFeature(nullptr, report.size()));
    FdReportWriter closed(-1, &writeMtx);
    CHECK_FALSE(closed.writeFeature(report.data(), report.size()));
}
