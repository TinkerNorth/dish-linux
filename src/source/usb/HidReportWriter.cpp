// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/usb/HidReportWriter.h"

#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>

namespace dish::source::usb {

// hidraw takes the report exactly as built, with the id as byte 0 and no padding: unlike the
// Windows HID stack it does not demand a fixed-length buffer, so the builder's length IS the
// transfer length.
bool FdReportWriter::write(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0 || fd_ < 0) { return false; }
    std::lock_guard<thread::TrackedMutex> lock(*writeMtx_);
    for (;;) {
        thread::blockingRegion("hid.write");
        const ssize_t written = ::write(fd_, data, len);
        if (written >= 0) { return static_cast<std::size_t>(written) == len; }
        // A signal during the write is not a failure; anything else is. Retrying EINTR here
        // matters because the app installs handlers (crash reporting, session management) that
        // can land on any thread.
        if (errno == EINTR) { continue; }
        // Silent: a feedback write is lossy telemetry, and a pad that has gone away is about to
        // surface as a claim failure anyway.
        return false;
    }
}

// The ioctl takes a writable buffer, so the report is copied rather than cast; its length is the
// transfer length, as a write's is. A signal is retried like the write's.
FeatureWrite FdReportWriter::writeFeature(const std::uint8_t* data, std::size_t len) {
    std::array<std::uint8_t, 256> buf{};
    if (data == nullptr || len == 0 || len > buf.size() || fd_ < 0) {
        return FeatureWrite::Refused;
    }
    std::memcpy(buf.data(), data, len);
    std::lock_guard<thread::TrackedMutex> lock(*writeMtx_);
    for (;;) {
        thread::blockingRegion("hid.write");
        if (::ioctl(fd_, HIDIOCSFEATURE(static_cast<unsigned>(len)), buf.data()) >= 0) {
            return FeatureWrite::Taken;
        }
        if (errno == EINTR) { continue; }
        return featureWriteOutcomeOf(errno);
    }
}

} // namespace dish::source::usb
