// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The XUSB <-> HID button/hat bit map. The result is packed into one `int` (low
// 16 bits = HID button word, bits 16..19 = hat octant) rather than a struct, to
// keep the <=250 Hz forwarding path allocation-free. The constants below are the
// satellite/Android HID report layout and must not drift from it. Unknown XUSB
// bits are dropped, so xusbToHid then hidToXusb round-trips every canonical bit.

#pragma once

namespace dish::input::layout {

// XInput (XUSB) wButtons bits.
inline constexpr int kXusbDpadUp = 0x0001;
inline constexpr int kXusbDpadDown = 0x0002;
inline constexpr int kXusbDpadLeft = 0x0004;
inline constexpr int kXusbDpadRight = 0x0008;
inline constexpr int kXusbStart = 0x0010;
inline constexpr int kXusbBack = 0x0020;
inline constexpr int kXusbLeftThumb = 0x0040;
inline constexpr int kXusbRightThumb = 0x0080;
inline constexpr int kXusbLeftShoulder = 0x0100;
inline constexpr int kXusbRightShoulder = 0x0200;
inline constexpr int kXusbGuide = 0x0400;
inline constexpr int kXusbA = 0x1000;
inline constexpr int kXusbB = 0x2000;
inline constexpr int kXusbX = 0x4000;
inline constexpr int kXusbY = 0x8000;

inline constexpr int kXusbDpadMask = 0x000F;

// HID report button bits.
inline constexpr int kHidA = 0x0001;
inline constexpr int kHidB = 0x0002;
inline constexpr int kHidX = 0x0004;
inline constexpr int kHidY = 0x0008;
inline constexpr int kHidLb = 0x0010;
inline constexpr int kHidRb = 0x0020;
inline constexpr int kHidSelect = 0x0040;
inline constexpr int kHidStart = 0x0080;
inline constexpr int kHidLs = 0x0100;
inline constexpr int kHidRs = 0x0200;
inline constexpr int kHidHome = 0x0400;

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
inline int dpadBitsToHat(int dpadBits) {
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

inline int hatToDpadBits(int hat) {
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
        return 0;
    }
}

inline int xusbToHid(int wButtons) {
    const int hat = dpadBitsToHat(wButtons & kXusbDpadMask);
    int hid = 0;
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
    return (hat << 16) | (hid & 0xFFFF);
}

inline int hidButtonsOf(int packed) { return packed & 0xFFFF; }

inline int hidHatOf(int packed) { return (packed >> 16) & 0xF; }

inline int hidToXusb(int hidButtons, int hat) {
    int w = hatToDpadBits(hat);
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
