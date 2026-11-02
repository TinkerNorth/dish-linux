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
using namespace std::chrono_literals;

namespace {

// A pad that takes `delay` to accept each report, recording what it got.
struct SlowPad {
    std::chrono::milliseconds delay{0};
    mutable std::mutex mtx;
    std::vector<std::vector<std::uint8_t>> reports;

    std::vector<std::vector<std::uint8_t>> got() const {
        const std::lock_guard<std::mutex> lock(mtx);
        return reports;
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

  private:
    SlowPad& pad_;
};

bool waitFor(const SlowPad& pad, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pad.got().size() >= count) { return true; }
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
