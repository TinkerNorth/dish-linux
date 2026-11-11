// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "core/thread/TrackedMutex.h"

#include <cstddef>
#include <cstdint>

namespace dish::source::usb {

// One claimed pad's OUT reports, written by the claim's output worker and nobody else on the hot
// path. Blocking: a call returns once the pad took the report or the write was given up.
class HidReportWriter {
  public:
    virtual ~HidReportWriter() = default;
    virtual bool write(const std::uint8_t* data, std::size_t len) = 0;
    // One feature report, `data[0]` its id, the whole report as built. False for a writer with no
    // feature path, which is every writer but a claimed pad's own.
    virtual bool writeFeature(const std::uint8_t* /*data*/, std::size_t /*len*/) { return false; }

  protected:
    HidReportWriter() = default;
    HidReportWriter(const HidReportWriter&) = default;
    HidReportWriter& operator=(const HidReportWriter&) = default;
    HidReportWriter(HidReportWriter&&) = default;
    HidReportWriter& operator=(HidReportWriter&&) = default;
};

// The real one: a write to the claim's hidraw node, the report exactly as built, and a feature
// report through HIDIOCSFEATURE the same way. `writeMtx` is the claim's own (hid.write), shared
// with the gateway's other writes to the node; the node stays open until the worker that owns
// this writer has stopped.
class FdReportWriter final : public HidReportWriter {
  public:
    FdReportWriter(int fd, thread::TrackedMutex* writeMtx) : fd_(fd), writeMtx_(writeMtx) {}

    bool write(const std::uint8_t* data, std::size_t len) override;
    bool writeFeature(const std::uint8_t* data, std::size_t len) override;

  private:
    const int fd_;
    thread::TrackedMutex* const writeMtx_;
};

} // namespace dish::source::usb
