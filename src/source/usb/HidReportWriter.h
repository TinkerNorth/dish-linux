// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "core/thread/TrackedMutex.h"

#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace dish::source::usb {

// What a feature write came to: the pad took the report, refused it (which a dongle under load
// does transiently, so writeFeatureWithRetry offers it again), or is gone, which no retry mends.
enum class FeatureWrite : std::uint8_t {
    Taken,
    Refused,
    PadGone,
};

// One claimed pad's OUT reports, written by the claim's output worker and nobody else on the hot
// path. Blocking: a call returns once the pad took the report or the write was given up.
class HidReportWriter {
  public:
    virtual ~HidReportWriter() = default;
    virtual bool write(const std::uint8_t* data, std::size_t len) = 0;
    // One feature report, `data[0]` its id, the whole report as built. Refused by a writer with no
    // feature path, which is every writer but a claimed pad's own.
    virtual FeatureWrite writeFeature(const std::uint8_t* /*data*/, std::size_t /*len*/) {
        return FeatureWrite::Refused;
    }

  protected:
    HidReportWriter() = default;
    HidReportWriter(const HidReportWriter&) = default;
    HidReportWriter& operator=(const HidReportWriter&) = default;
    HidReportWriter(HidReportWriter&&) = default;
    HidReportWriter& operator=(HidReportWriter&&) = default;
};

inline constexpr std::chrono::milliseconds kFeatureWriteRetryPause{20};

// What a failed HIDIOCSFEATURE's errno says: the node's device left (ENODEV, ENXIO), which no
// retry mends, or anything else.
inline FeatureWrite featureWriteOutcomeOf(int error) {
    const bool gone = error == ENODEV || error == ENXIO;
    return gone ? FeatureWrite::PadGone : FeatureWrite::Refused;
}

// A feature report offered `attempts` times in all, with a pause after each refusal: the way SDL
// and hid-steam retry the 2015 dongle's EPIPE under load (usbparse::configWriteAttempts says how
// often a family's reports are offered). True once the pad took it; a pad that is gone is not
// asked again.
inline bool writeFeatureWithRetry(HidReportWriter& writer, const std::uint8_t* report,
                                  std::size_t len, int attempts) {
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        switch (writer.writeFeature(report, len)) {
        case FeatureWrite::Taken:
            return true;
        case FeatureWrite::PadGone:
            return false;
        case FeatureWrite::Refused:
            break;
        }
        const bool lastAttempt = attempt == attempts;
        if (!lastAttempt) { std::this_thread::sleep_for(kFeatureWriteRetryPause); }
    }
    return false;
}

// The real one: a write to the claim's hidraw node, the report exactly as built, and a feature
// report through HIDIOCSFEATURE the same way. `writeMtx` is the claim's own (hid.write), shared
// with the gateway's other writes to the node; the node stays open until the worker that owns
// this writer has stopped.
class FdReportWriter final : public HidReportWriter {
  public:
    FdReportWriter(int fd, thread::TrackedMutex* writeMtx) : fd_(fd), writeMtx_(writeMtx) {}

    bool write(const std::uint8_t* data, std::size_t len) override;
    FeatureWrite writeFeature(const std::uint8_t* data, std::size_t len) override;

  private:
    const int fd_;
    thread::TrackedMutex* const writeMtx_;
};

} // namespace dish::source::usb
