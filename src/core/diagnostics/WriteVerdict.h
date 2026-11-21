// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The client-to-pad level of Diagnostics (docs/controller-db.md section 9):
// the last Direct write's verdict, per pad. The output worker records it after
// each write, on its own thread and without allocating, so the value is a
// trivially copyable record a seqlock can carry; the page and the bundle word
// it from here.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace dish::diagnostics {

// How the report left the host. An output report goes through write() on the hidraw node, which
// usbhid sends over the interrupt OUT pipe; a feature report goes through HIDIOCSFEATURE, a
// SET_REPORT control transfer.
enum class WriteRoute : std::uint8_t { InterruptOut, SetReport };

struct WriteVerdict {
    // False until the pad has been written once.
    bool written = false;
    WriteRoute route = WriteRoute::InterruptOut;
    std::uint8_t reportId = 0;
    bool ok = false;
    // errno after a failed write, 0 for a success or a failure the kernel gave no code for (a short
    // write).
    std::uint32_t osError = 0;
    // Wall-clock milliseconds since the epoch.
    std::int64_t atMs = 0;

    bool operator==(const WriteVerdict&) const = default;
};

static_assert(std::is_trivially_copyable_v<WriteVerdict>, "a seqlock copies it word by word");

inline WriteVerdict verdictOf(WriteRoute route, const std::uint8_t* report, std::size_t len,
                              bool ok, std::uint32_t osError, std::int64_t atMs) {
    const std::uint8_t reportId = (report != nullptr && len > 0) ? report[0] : 0;
    return WriteVerdict{true, route, reportId, ok, ok ? 0U : osError, atMs};
}

inline std::string_view writeRouteToken(WriteRoute route) {
    switch (route) {
    case WriteRoute::InterruptOut:
        return "interruptOut";
    case WriteRoute::SetReport:
        return "setReport";
    }
    return "interruptOut";
}

struct OsErrorName {
    std::uint32_t code;
    std::string_view name;
};

// The errno values a hidraw write or HIDIOCSFEATURE comes back with, by their <errno.h> names.
// Spelled as Linux's numbers rather than the macros so this header stays platform-free; the
// test pins each one against the macro of the same name.
inline constexpr std::array<OsErrorName, 17> kOsErrorNames{{
    {1, "EPERM"},
    {2, "ENOENT"},
    {4, "EINTR"},
    {5, "EIO"},
    {6, "ENXIO"},
    {9, "EBADF"},
    {11, "EAGAIN"},
    {13, "EACCES"},
    {16, "EBUSY"},
    {19, "ENODEV"},
    {22, "EINVAL"},
    {25, "ENOTTY"},
    {32, "EPIPE"},
    {71, "EPROTO"},
    {75, "EOVERFLOW"},
    {108, "ESHUTDOWN"},
    {110, "ETIMEDOUT"},
}};

// Empty for no error; the errno name where it is one of the above; else "errno:<code>".
inline std::string osErrorName(std::uint32_t code) {
    if (code == 0) { return {}; }
    for (const OsErrorName& entry : kOsErrorNames) {
        if (entry.code == code) { return std::string(entry.name); }
    }
    return "errno:" + std::to_string(code);
}

// A byte the way the bundle writes it, a report id or an endpoint address: two lowercase hex
// digits.
inline std::string hexByteText(std::uint8_t value) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out(2, '0');
    out[0] = kDigits[static_cast<std::size_t>(value >> 4)];
    out[1] = kDigits[static_cast<std::size_t>(value & 0x0F)];
    return out;
}

inline std::string reportIdText(std::uint8_t reportId) { return hexByteText(reportId); }

} // namespace dish::diagnostics
