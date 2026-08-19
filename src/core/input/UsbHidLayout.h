// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The gamepad field map distilled from a HID report descriptor, plus the
// bit-exact decoder over it. The fixed-offset fallback decoder only fits pads
// that happen to use the canonical packing, so a real layout is derived per
// device instead. hidraw hands back the raw report descriptor (HIDIOCGRDESC),
// so parseReportDescriptor below is what production actually runs, and the
// shared cross-client decode vectors pin it directly. decodeFromLayout is
// templated on the output state to avoid an include cycle with
// UsbReportParsers.h.

#pragma once

#include "core/input/GamepadButtonLayouts.h"

#include <cstddef>
#include <cstdint>

namespace dish::input::usbhid {

struct HidAxis {
    bool present = false;
    std::uint16_t bitOffset = 0;
    std::uint8_t bitSize = 0;
    std::int32_t logicalMin = 0;
    std::int32_t logicalMax = 0;
};

// Where each stick/trigger/hat/button lives within the input report. Fixed-size,
// no heap, so it can be stored per device and read on the report hot path.
struct HidLayout {
    bool valid = false;
    std::uint8_t reportId = 0; // 0 means the device sends no report-id prefix byte
    HidAxis lx, ly, rx, ry, lt, rt;
    bool hasHat = false;
    std::uint16_t hatBitOffset = 0;
    std::uint8_t hatBitSize = 0;
    std::int32_t hatLogicalMin = 0;
    std::int32_t hatLogicalMax = 0;
    std::uint16_t buttonBitOffset = 0;
    std::uint8_t buttonCount = 0;
    // Set by the attach path from the model catalog, after parseReportDescriptor
    // resets the struct; never derived from the descriptor itself.
    bool switchOrderButtons = false;
};

inline constexpr std::size_t kMaxUsages = 16;
inline constexpr std::uint8_t kMaxButtons = 16;

inline std::int32_t signExtend(std::uint32_t v, std::uint8_t bytes) {
    if (bytes == 0 || bytes >= 4) { return static_cast<std::int32_t>(v); }
    const std::uint32_t bits = bytes * 8u;
    const std::uint32_t signBit = 1u << (bits - 1);
    if (v & signBit) { return static_cast<std::int32_t>(v | ~((1u << bits) - 1u)); }
    return static_cast<std::int32_t>(v);
}

inline std::uint32_t extractBits(const std::uint8_t* d, std::size_t dlen, std::uint32_t bitOff,
                                 std::uint8_t bits) {
    std::uint32_t v = 0;
    for (std::uint8_t i = 0; i < bits && i < 32; i++) {
        const std::uint32_t bi = bitOff + i;
        if (static_cast<std::size_t>(bi >> 3) >= dlen) { break; }
        if ((d[bi >> 3] >> (bi & 7u)) & 1u) { v |= (1u << i); }
    }
    return v;
}

inline std::int32_t toSigned(std::uint32_t raw, std::uint8_t bits, std::int32_t logicalMin) {
    if (logicalMin < 0 && bits > 0 && bits < 32) {
        const std::uint32_t signBit = 1u << (bits - 1);
        if (raw & signBit) { return static_cast<std::int32_t>(raw | ~((1u << bits) - 1u)); }
    }
    return static_cast<std::int32_t>(raw);
}

inline std::int16_t scaleAxis16(std::uint32_t raw, const HidAxis& a, bool invert) {
    const std::int32_t v = toSigned(raw, a.bitSize, a.logicalMin);
    const std::int32_t center = (a.logicalMin + a.logicalMax) / 2;
    const std::int32_t half = (a.logicalMax - a.logicalMin) / 2;
    if (half <= 0) { return 0; }
    std::int32_t scaled =
        static_cast<std::int32_t>(static_cast<std::int64_t>(v - center) * 32767 / half);
    if (invert) { scaled = -scaled; }
    if (scaled > 32767) { scaled = 32767; }
    if (scaled < -32768) { scaled = -32768; }
    return static_cast<std::int16_t>(scaled);
}

inline std::uint8_t scaleTrig8(std::uint32_t raw, const HidAxis& a) {
    const std::int32_t v = toSigned(raw, a.bitSize, a.logicalMin);
    const std::int32_t span = a.logicalMax - a.logicalMin;
    if (span <= 0) { return 0; }
    std::int32_t scaled =
        static_cast<std::int32_t>(static_cast<std::int64_t>(v - a.logicalMin) * 255 / span);
    if (scaled < 0) { scaled = 0; }
    if (scaled > 255) { scaled = 255; }
    return static_cast<std::uint8_t>(scaled);
}

// Button 1 = A through button 11 = Guide, the convention the fixed-offset
// fallback and the other clients also use.
inline std::uint16_t layoutButtonBit(std::uint8_t idx) {
    switch (idx) {
    case 0:
        return static_cast<std::uint16_t>(layout::kXusbA);
    case 1:
        return static_cast<std::uint16_t>(layout::kXusbB);
    case 2:
        return static_cast<std::uint16_t>(layout::kXusbX);
    case 3:
        return static_cast<std::uint16_t>(layout::kXusbY);
    case 4:
        return static_cast<std::uint16_t>(layout::kXusbLeftShoulder);
    case 5:
        return static_cast<std::uint16_t>(layout::kXusbRightShoulder);
    case 6:
        return static_cast<std::uint16_t>(layout::kXusbBack);
    case 7:
        return static_cast<std::uint16_t>(layout::kXusbStart);
    case 8:
        return static_cast<std::uint16_t>(layout::kXusbLeftThumb);
    case 9:
        return static_cast<std::uint16_t>(layout::kXusbRightThumb);
    case 10:
        return static_cast<std::uint16_t>(layout::kXusbGuide);
    default:
        return 0;
    }
}

// Switch-order HID pads declare buttons in usage row Y B A X L R ZL ZR Minus
// Plus L3 R3 Home Capture; remap by position to match decodeSwitchProUsb. ZL/ZR
// (indices 6/7) fold into the triggers in decodeFromLayout instead of mapping
// here.
inline std::uint16_t switchOrderButtonBit(std::uint8_t idx) {
    switch (idx) {
    case 0:
        return static_cast<std::uint16_t>(layout::kXusbX);
    case 1:
        return static_cast<std::uint16_t>(layout::kXusbA);
    case 2:
        return static_cast<std::uint16_t>(layout::kXusbB);
    case 3:
        return static_cast<std::uint16_t>(layout::kXusbY);
    case 4:
        return static_cast<std::uint16_t>(layout::kXusbLeftShoulder);
    case 5:
        return static_cast<std::uint16_t>(layout::kXusbRightShoulder);
    case 8:
        return static_cast<std::uint16_t>(layout::kXusbBack);
    case 9:
        return static_cast<std::uint16_t>(layout::kXusbStart);
    case 10:
        return static_cast<std::uint16_t>(layout::kXusbLeftThumb);
    case 11:
        return static_cast<std::uint16_t>(layout::kXusbRightThumb);
    case 12:
        return static_cast<std::uint16_t>(layout::kXusbGuide);
    default:
        return 0;
    }
}

inline std::uint16_t dpadBitsForDir(int dir) {
    switch (dir) {
    case 0:
        return static_cast<std::uint16_t>(layout::kXusbDpadUp);
    case 1:
        return static_cast<std::uint16_t>(layout::kXusbDpadUp | layout::kXusbDpadRight);
    case 2:
        return static_cast<std::uint16_t>(layout::kXusbDpadRight);
    case 3:
        return static_cast<std::uint16_t>(layout::kXusbDpadDown | layout::kXusbDpadRight);
    case 4:
        return static_cast<std::uint16_t>(layout::kXusbDpadDown);
    case 5:
        return static_cast<std::uint16_t>(layout::kXusbDpadDown | layout::kXusbDpadLeft);
    case 6:
        return static_cast<std::uint16_t>(layout::kXusbDpadLeft);
    case 7:
        return static_cast<std::uint16_t>(layout::kXusbDpadUp | layout::kXusbDpadLeft);
    default:
        return 0;
    }
}

// First declaration of an axis wins. Public because the caps-derived builder in
// HidLayoutFromCaps must assign fields through the same rules.
inline void setAxis(HidAxis& a, std::uint32_t bit, std::uint32_t size, std::int32_t lo,
                    std::int32_t hi) {
    if (a.present) { return; }
    a.present = true;
    a.bitOffset = static_cast<std::uint16_t>(bit);
    a.bitSize = static_cast<std::uint8_t>(size);
    a.logicalMin = lo;
    a.logicalMax = hi;
}

// Generic Desktop right stick is Z/Rz and triggers are Rx/Ry, matching what the
// fixed-offset fallback assumes; Simulation Brake/Accelerator also feed triggers.
inline void assignUsage(HidLayout& out, std::uint32_t page, std::uint32_t usage, std::uint32_t bit,
                        std::uint32_t size, std::int32_t lo, std::int32_t hi) {
    if (page == 0x01) {
        switch (usage) {
        case 0x30:
            setAxis(out.lx, bit, size, lo, hi);
            break;
        case 0x31:
            setAxis(out.ly, bit, size, lo, hi);
            break;
        case 0x32:
            setAxis(out.rx, bit, size, lo, hi);
            break;
        case 0x35:
            setAxis(out.ry, bit, size, lo, hi);
            break;
        case 0x33:
            setAxis(out.lt, bit, size, lo, hi);
            break;
        case 0x34:
            setAxis(out.rt, bit, size, lo, hi);
            break;
        case 0x39:
            if (!out.hasHat) {
                out.hasHat = true;
                out.hatBitOffset = static_cast<std::uint16_t>(bit);
                out.hatBitSize = static_cast<std::uint8_t>(size);
                out.hatLogicalMin = lo;
                out.hatLogicalMax = hi;
            }
            break;
        default:
            break;
        }
    } else if (page == 0x02) {
        if (usage == 0xC5) {
            setAxis(out.lt, bit, size, lo, hi);
        } else if (usage == 0xC4) {
            setAxis(out.rt, bit, size, lo, hi);
        }
    }
}

// Returns false and leaves the layout invalid on malformed input or when nothing
// gamepad-like is found, so callers fall back to a fixed-offset guess.
inline bool parseReportDescriptor(const std::uint8_t* desc, std::size_t len, HidLayout& out) {
    out = HidLayout{};

    std::uint32_t usagePage = 0, reportSize = 0, reportCount = 0;
    std::int32_t logMin = 0, logMax = 0;
    std::uint8_t currentReportId = 0;
    std::uint32_t bitCursor = 0;
    bool locked = false;
    std::uint8_t lockedReportId = 0;

    std::uint32_t usages[kMaxUsages];
    std::size_t usageCount = 0;
    std::uint32_t usageMin = 0;
    bool haveRange = false;

    std::size_t i = 0;
    while (i < len) {
        const std::uint8_t prefix = desc[i++];
        if (prefix == 0xFE) { // long item: 1 size byte + 1 tag byte + payload
            if (i >= len) { break; }
            const std::uint8_t payload = desc[i];
            i += 2u + payload;
            continue;
        }
        const std::uint8_t bSize = prefix & 0x03u;
        const std::uint8_t dataLen = bSize == 3 ? 4 : bSize;
        const std::uint8_t bType = (prefix >> 2) & 0x03u;
        const std::uint8_t bTag = (prefix >> 4) & 0x0Fu;
        if (i + dataLen > len) { break; }
        std::uint32_t data = 0;
        for (std::uint8_t k = 0; k < dataLen; k++) {
            data |= static_cast<std::uint32_t>(desc[i + k]) << (8u * k);
        }
        i += dataLen;

        if (bType == 0) {      // Main
            if (bTag == 0x8) { // Input
                const std::uint32_t startBit = bitCursor;
                bitCursor += reportSize * reportCount;
                const bool isConst = (data & 0x01u) != 0;
                if (!isConst && reportSize > 0 && reportCount > 0) {
                    if (!locked) {
                        locked = true;
                        lockedReportId = currentReportId;
                        out.reportId = currentReportId;
                    }
                    if (currentReportId == lockedReportId) {
                        if (usagePage == 0x09) {
                            if (out.buttonCount == 0) {
                                out.buttonBitOffset = static_cast<std::uint16_t>(startBit);
                                const std::uint32_t cnt =
                                    reportCount > kMaxButtons ? kMaxButtons : reportCount;
                                out.buttonCount = static_cast<std::uint8_t>(cnt);
                            }
                        } else if (usagePage == 0x01 || usagePage == 0x02) {
                            for (std::uint32_t f = 0; f < reportCount; f++) {
                                std::uint32_t usage;
                                if (haveRange) {
                                    usage = usageMin + f;
                                } else if (usageCount == 0) {
                                    break;
                                } else {
                                    usage = usages[f < usageCount ? f : usageCount - 1];
                                }
                                assignUsage(out, usagePage, usage, startBit + f * reportSize,
                                            reportSize, logMin, logMax);
                            }
                        }
                    }
                }
            }
            usageCount = 0;
            haveRange = false;
            usageMin = 0;
        } else if (bType == 1) { // Global
            switch (bTag) {
            case 0x0:
                usagePage = data;
                break;
            case 0x1:
                logMin = signExtend(data, dataLen);
                break;
            case 0x2:
                logMax = signExtend(data, dataLen);
                break;
            case 0x7:
                reportSize = data;
                break;
            case 0x8:
                currentReportId = static_cast<std::uint8_t>(data);
                bitCursor = 0;
                break;
            case 0x9:
                reportCount = data;
                break;
            default:
                break;
            }
        } else if (bType == 2) { // Local
            switch (bTag) {
            case 0x0:
                if (usageCount < kMaxUsages) { usages[usageCount++] = data; }
                break;
            case 0x1:
                usageMin = data;
                haveRange = true;
                break;
            case 0x2:
                haveRange = true;
                break;
            default:
                break;
            }
        }
    }

    out.valid = out.lx.present || out.ly.present || out.buttonCount > 0 || out.hasHat;
    return out.valid;
}

// StateT supplies wButtons, lt/rt and the four stick fields. Bit offsets are
// relative to the post-id payload when reportId != 0 and to the buffer start
// otherwise. hidraw delivers the report exactly as the device sent it, so no
// id byte has to be stripped first.
template <typename StateT>
inline bool decodeFromLayout(const std::uint8_t* buf, std::size_t len, StateT& s,
                             const HidLayout& L) {
    if (!L.valid) { return false; }
    std::size_t dataStart = 0;
    if (L.reportId != 0) {
        if (len < 1 || buf[0] != L.reportId) { return false; }
        dataStart = 1;
    }
    const std::uint8_t* d = buf + dataStart;
    const std::size_t dlen = len - dataStart;

    if (L.lx.present) {
        s.lx = scaleAxis16(extractBits(d, dlen, L.lx.bitOffset, L.lx.bitSize), L.lx, false);
    }
    if (L.ly.present) {
        s.ly = scaleAxis16(extractBits(d, dlen, L.ly.bitOffset, L.ly.bitSize), L.ly, true);
    }
    if (L.rx.present) {
        s.rx = scaleAxis16(extractBits(d, dlen, L.rx.bitOffset, L.rx.bitSize), L.rx, false);
    }
    if (L.ry.present) {
        s.ry = scaleAxis16(extractBits(d, dlen, L.ry.bitOffset, L.ry.bitSize), L.ry, true);
    }
    if (L.lt.present) {
        s.lt = scaleTrig8(extractBits(d, dlen, L.lt.bitOffset, L.lt.bitSize), L.lt);
    }
    if (L.rt.present) {
        s.rt = scaleTrig8(extractBits(d, dlen, L.rt.bitOffset, L.rt.bitSize), L.rt);
    }

    std::uint16_t b = 0;
    if (L.hasHat) {
        const std::uint32_t raw = extractBits(d, dlen, L.hatBitOffset, L.hatBitSize);
        const int dir = static_cast<int>(raw) - static_cast<int>(L.hatLogicalMin);
        const int range = static_cast<int>(L.hatLogicalMax) - static_cast<int>(L.hatLogicalMin);
        if (dir >= 0 && dir <= range && dir <= 7) {
            b = static_cast<std::uint16_t>(b | dpadBitsForDir(dir));
        }
    }
    if (L.switchOrderButtons) {
        bool zl = false;
        bool zr = false;
        for (std::uint8_t i = 0; i < L.buttonCount; i++) {
            if (!extractBits(d, dlen, static_cast<std::uint32_t>(L.buttonBitOffset) + i, 1)) {
                continue;
            }
            if (i == 6) {
                zl = true;
            } else if (i == 7) {
                zr = true;
            } else {
                b = static_cast<std::uint16_t>(b | switchOrderButtonBit(i));
            }
        }
        s.lt = zl ? 255 : 0;
        s.rt = zr ? 255 : 0;
    } else {
        for (std::uint8_t i = 0; i < L.buttonCount; i++) {
            if (extractBits(d, dlen, static_cast<std::uint32_t>(L.buttonBitOffset) + i, 1)) {
                b = static_cast<std::uint16_t>(b | layoutButtonBit(i));
            }
        }
    }
    s.wButtons = b;
    return true;
}

} // namespace dish::input::usbhid
