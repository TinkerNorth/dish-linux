// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The XUSB <-> HID button/hat bit map. The result is packed into one `std::uint32_t` (low
// 16 bits = HID button word, bits 16..19 = hat octant) rather than a struct, to
// keep the <=250 Hz forwarding path allocation-free. The constants below are the
// satellite/Android HID report layout and must not drift from it. Unknown XUSB
// bits are dropped, so xusbToHid then hidToXusb round-trips every canonical bit.

#pragma once

#include <cstdint>

namespace dish::input::layout {

// XInput (XUSB) wButtons bits.
inline constexpr std::uint32_t kXusbDpadUp = 0x0001U;
inline constexpr std::uint32_t kXusbDpadDown = 0x0002U;
inline constexpr std::uint32_t kXusbDpadLeft = 0x0004U;
inline constexpr std::uint32_t kXusbDpadRight = 0x0008U;
inline constexpr std::uint32_t kXusbStart = 0x0010U;
inline constexpr std::uint32_t kXusbBack = 0x0020U;
inline constexpr std::uint32_t kXusbLeftThumb = 0x0040U;
inline constexpr std::uint32_t kXusbRightThumb = 0x0080U;
inline constexpr std::uint32_t kXusbLeftShoulder = 0x0100U;
inline constexpr std::uint32_t kXusbRightShoulder = 0x0200U;
inline constexpr std::uint32_t kXusbGuide = 0x0400U;
// Not an XINPUT bit: 0x0800 is the one value the XINPUT-shaped word leaves
// unassigned, and protocol 2 spends it on the DualSense mic-mute button
// (satellite core/types.h WBUTTON_MIC_MUTE, docs/contract.md, Controller
// audio). It carries mute STATE, not an edge, held on every frame while muted,
// and only the DualSense identity consumes it host-side; every other emulated
// pad ignores it. xusbToHid deliberately drops it (no HID gamepad descriptor
// has a mute button, and Moonlight must never see it) and hidToXusb never
// produces it, so it can only be set by an explicit fold. Wave 2 of the
// controller-audio port is what sets it on live reports.
inline constexpr std::uint32_t kXusbMicMute = 0x0800U;
inline constexpr std::uint32_t kXusbA = 0x1000U;
inline constexpr std::uint32_t kXusbB = 0x2000U;
inline constexpr std::uint32_t kXusbX = 0x4000U;
inline constexpr std::uint32_t kXusbY = 0x8000U;

inline constexpr std::uint32_t kXusbDpadMask = 0x000FU;

// HID report button bits.
inline constexpr std::uint32_t kHidA = 0x0001U;
inline constexpr std::uint32_t kHidB = 0x0002U;
inline constexpr std::uint32_t kHidX = 0x0004U;
inline constexpr std::uint32_t kHidY = 0x0008U;
inline constexpr std::uint32_t kHidLb = 0x0010U;
inline constexpr std::uint32_t kHidRb = 0x0020U;
inline constexpr std::uint32_t kHidSelect = 0x0040U;
inline constexpr std::uint32_t kHidStart = 0x0080U;
inline constexpr std::uint32_t kHidLs = 0x0100U;
inline constexpr std::uint32_t kHidRs = 0x0200U;
inline constexpr std::uint32_t kHidHome = 0x0400U;

// Hat octants run clockwise from N=1; 0 is neutral.
inline constexpr int kHatNeutral = 0;
inline constexpr int kHatN = 1;
inline constexpr int kHatNe = 2;
inline constexpr int kHatE = 3;
inline constexpr int kHatSe = 4;
inline constexpr int kHatS = 5;
inline constexpr int kHatSw = 6;
inline constexpr int kHatW = 7;
inline constexpr int kHatNw = 8;

// Diagonals take precedence over cardinals, and a physically impossible opposing
// pair falls through to the remaining axis or neutral. The test order is the
// contract: it must match the other clients so the same combo folds identically.
inline int dpadBitsToHat(std::uint32_t dpadBits) {
    const bool up = (dpadBits & kXusbDpadUp) != 0;
    const bool down = (dpadBits & kXusbDpadDown) != 0;
    const bool left = (dpadBits & kXusbDpadLeft) != 0;
    const bool right = (dpadBits & kXusbDpadRight) != 0;
    if (up && right) { return kHatNe; }
    if (right && down) { return kHatSe; }
    if (down && left) { return kHatSw; }
    if (left && up) { return kHatNw; }
    if (up) { return kHatN; }
    if (right) { return kHatE; }
    if (down) { return kHatS; }
    if (left) { return kHatW; }
    return kHatNeutral;
}

inline std::uint32_t hatToDpadBits(int hat) {
    switch (hat) {
    case kHatN:
        return kXusbDpadUp;
    case kHatNe:
        return kXusbDpadUp | kXusbDpadRight;
    case kHatE:
        return kXusbDpadRight;
    case kHatSe:
        return kXusbDpadRight | kXusbDpadDown;
    case kHatS:
        return kXusbDpadDown;
    case kHatSw:
        return kXusbDpadDown | kXusbDpadLeft;
    case kHatW:
        return kXusbDpadLeft;
    case kHatNw:
        return kXusbDpadLeft | kXusbDpadUp;
    default:
        return 0U;
    }
}

inline std::uint32_t xusbToHid(std::uint32_t wButtons) {
    const int hat = dpadBitsToHat(wButtons & kXusbDpadMask);
    std::uint32_t hid = 0U;
    if ((wButtons & kXusbStart) != 0) { hid |= kHidStart; }
    if ((wButtons & kXusbBack) != 0) { hid |= kHidSelect; }
    if ((wButtons & kXusbLeftThumb) != 0) { hid |= kHidLs; }
    if ((wButtons & kXusbRightThumb) != 0) { hid |= kHidRs; }
    if ((wButtons & kXusbLeftShoulder) != 0) { hid |= kHidLb; }
    if ((wButtons & kXusbRightShoulder) != 0) { hid |= kHidRb; }
    if ((wButtons & kXusbGuide) != 0) { hid |= kHidHome; }
    if ((wButtons & kXusbA) != 0) { hid |= kHidA; }
    if ((wButtons & kXusbB) != 0) { hid |= kHidB; }
    if ((wButtons & kXusbX) != 0) { hid |= kHidX; }
    if ((wButtons & kXusbY) != 0) { hid |= kHidY; }
    return (static_cast<std::uint32_t>(hat) << 16U) | (hid & 0xFFFFU);
}

inline std::uint32_t hidButtonsOf(std::uint32_t packed) { return packed & 0xFFFFU; }

inline int hidHatOf(std::uint32_t packed) { return static_cast<int>((packed >> 16U) & 0xFU); }

inline std::uint32_t hidToXusb(std::uint32_t hidButtons, int hat) {
    std::uint32_t w = hatToDpadBits(hat);
    if ((hidButtons & kHidStart) != 0) { w |= kXusbStart; }
    if ((hidButtons & kHidSelect) != 0) { w |= kXusbBack; }
    if ((hidButtons & kHidLs) != 0) { w |= kXusbLeftThumb; }
    if ((hidButtons & kHidRs) != 0) { w |= kXusbRightThumb; }
    if ((hidButtons & kHidLb) != 0) { w |= kXusbLeftShoulder; }
    if ((hidButtons & kHidRb) != 0) { w |= kXusbRightShoulder; }
    if ((hidButtons & kHidHome) != 0) { w |= kXusbGuide; }
    if ((hidButtons & kHidA) != 0) { w |= kXusbA; }
    if ((hidButtons & kHidB) != 0) { w |= kXusbB; }
    if ((hidButtons & kHidX) != 0) { w |= kXusbX; }
    if ((hidButtons & kHidY) != 0) { w |= kXusbY; }
    return w;
}

} // namespace dish::input::layout
