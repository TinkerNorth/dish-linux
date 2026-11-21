// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// "Capture this controller" (docs/diagnostics-bundle.md, Capture, format usb-descriptors-v1): the
// next twenty input reports of one claimed pad, as they arrived, into slots fixed with the claim,
// then the capture disarms itself. dish-android's ReportCapture (usb_device_table.h) is the
// pattern; here the armed flag shares one word with the read loop's own running flag, so a loop
// with no capture armed pays the one relaxed load per report it already paid, and nothing else.

#pragma once

#include "core/diagnostics/Json.h"
#include "core/diagnostics/UsbDescriptors.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dish::diagnostics {

inline constexpr std::size_t kCaptureReports = 20;
// The longest report kept whole; a longer one keeps its first bytes.
inline constexpr std::size_t kCaptureReportMaxBytes = 64;
inline constexpr std::string_view kCaptureFormat = "usb-descriptors-v1";

// The clock a read loop stamps a captured report with: monotonic, so a wall-clock step between
// the reports and the file cannot reorder them.
inline std::int64_t captureClockMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

struct CapturedReport {
    // On the clock the read loop stamped it with.
    std::int64_t atMs = 0;
    std::uint8_t length = 0;
    std::array<std::uint8_t, kCaptureReportMaxBytes> bytes{};
};

// The word a claim's read loop loads once per report: whether it keeps reading, and whether a
// capture is armed.
class ReadLoopWord {
  public:
    static constexpr std::uint8_t kRunning = 0x01;
    static constexpr std::uint8_t kCaptureArmed = 0x02;

    // The read loop's per-report load.
    std::uint8_t load() const { return word_.load(std::memory_order_relaxed); }
    // What a capture reads before it writes a slot, which sees the slots its arming reset.
    std::uint8_t loadAcquire() const { return word_.load(std::memory_order_acquire); }

    static bool running(std::uint8_t word) { return (word & kRunning) != 0; }
    static bool capturing(std::uint8_t word) { return (word & kCaptureArmed) != 0; }

    void start() { word_.fetch_or(kRunning, std::memory_order_release); }
    void stop() {
        word_.fetch_and(static_cast<std::uint8_t>(~kRunning), std::memory_order_release);
    }
    // True when this call armed it; false while a capture is already running.
    bool armCapture() {
        const std::uint8_t before = word_.fetch_or(kCaptureArmed, std::memory_order_acq_rel);
        return !capturing(before);
    }
    void disarmCapture() {
        word_.fetch_and(static_cast<std::uint8_t>(~kCaptureArmed), std::memory_order_release);
    }

  private:
    std::atomic<std::uint8_t> word_{0};
};

class ReportCapture {
  public:
    // Not the read loop's thread. False while a capture is still running.
    bool arm(ReadLoopWord& word) {
        if (ReadLoopWord::capturing(word.loadAcquire())) { return false; }
        filled_.store(0, std::memory_order_relaxed);
        return word.armCapture();
    }

    // The read loop's, for a report its per-report load found a capture armed for. The last slot
    // disarms the capture.
    void offer(ReadLoopWord& word, const std::uint8_t* bytes, std::size_t len, std::int64_t atMs) {
        if (!ReadLoopWord::capturing(word.loadAcquire())) { return; }
        const std::size_t i = filled_.load(std::memory_order_relaxed);
        if (i >= kCaptureReports) {
            word.disarmCapture();
            return;
        }
        CapturedReport& kept = reports_[i];
        const std::size_t keptLength = len < kCaptureReportMaxBytes ? len : kCaptureReportMaxBytes;
        kept.atMs = atMs;
        kept.length = static_cast<std::uint8_t>(keptLength);
        // A loop rather than memcpy: GCC's -Wstringop-overflow misreads a guarded slot.
        for (std::size_t b = 0; b < keptLength; ++b) { kept.bytes[b] = bytes[b]; }
        filled_.store(i + 1, std::memory_order_release);
        if (i + 1 == kCaptureReports) { word.disarmCapture(); }
    }

    // The reports kept so far, in arrival order.
    std::vector<CapturedReport> kept() const {
        const std::size_t filled = filled_.load(std::memory_order_acquire);
        std::vector<CapturedReport> out(reports_.begin(),
                                        reports_.begin() + static_cast<std::ptrdiff_t>(filled));
        return out;
    }

  private:
    std::atomic<std::size_t> filled_{0};
    std::array<CapturedReport, kCaptureReports> reports_{};
};

// Where a capture the person asked for stands, polled while it runs: done once twenty reports are
// in, or once its time is up with however many a pad that reports only on change sent at rest;
// failed when the pad stopped being read directly before either.
enum class CaptureProgress : std::uint8_t { Capturing, Captured, Failed };

inline CaptureProgress captureProgress(std::size_t kept, int polls, int maxPolls, bool claimLive) {
    if (!claimLive) { return CaptureProgress::Failed; }
    const bool full = kept >= kCaptureReports;
    const bool timeUp = polls >= maxPolls;
    return full || timeUp ? CaptureProgress::Captured : CaptureProgress::Capturing;
}

// A report as the file writes it: when it arrived on the wall clock, and its bytes.
struct RestReport {
    std::int64_t atMs = 0;
    DescriptorBytes bytes;

    bool operator==(const RestReport&) const = default;
};

// Each kept report's arrival moved from the read loop's clock onto the wall clock: its age on the
// first, read now, taken off the wall clock's now.
std::vector<RestReport> restReportsOf(const std::vector<CapturedReport>& kept,
                                      std::int64_t loopClockNowMs, std::int64_t wallClockNowMs);

// `capture` for the pad the person picked: the hex of its 18-byte device descriptor and of every
// configuration in index order, its strings by role (never the serial number), its report
// descriptor and the reports at rest. A field the platform could not read is null.
Json captureJson(const UsbHardwareFacts& hardware, const std::vector<RestReport>& reports);

} // namespace dish::diagnostics
