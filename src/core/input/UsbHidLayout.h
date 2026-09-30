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
    if ((v & signBit) != 0) { return static_cast<std::int32_t>(v | ~((1u << bits) - 1u)); }
    return static_cast<std::int32_t>(v);
}

inline std::uint32_t extractBits(const std::uint8_t* d, std::size_t dlen, std::uint32_t bitOff,
                                 std::uint8_t bits) {
    std::uint32_t v = 0;
    for (std::uint8_t i = 0; i < bits && i < 32; i++) {
        const std::uint32_t bi = bitOff + i;
        if (static_cast<std::size_t>(bi >> 3) >= dlen) { break; }
        if (((d[bi >> 3] >> (bi & 7u)) & 1u) != 0) { v |= (1u << i); }
    }
    return v;
}

inline std::int32_t toSigned(std::uint32_t raw, std::uint8_t bits, std::int32_t logicalMin) {
    if (logicalMin < 0 && bits > 0 && bits < 32) {
        const std::uint32_t signBit = 1u << (bits - 1);
        if ((raw & signBit) != 0) { return static_cast<std::int32_t>(raw | ~((1u << bits) - 1u)); }
    }
    return static_cast<std::int32_t>(raw);
}

inline std::int16_t scaleAxis16(std::uint32_t raw, const HidAxis& a, bool invert) {
    const std::int32_t v = toSigned(raw, a.bitSize, a.logicalMin);
    const std::int32_t center = (a.logicalMin + a.logicalMax) / 2;
    const std::int32_t half = (a.logicalMax - a.logicalMin) / 2;
    if (half <= 0) { return 0; }
    auto scaled = static_cast<std::int32_t>(static_cast<std::int64_t>(v - center) * 32767 / half);
    if (invert) { scaled = -scaled; }
    if (scaled > 32767) { scaled = 32767; }
    if (scaled < -32768) { scaled = -32768; }
    return static_cast<std::int16_t>(scaled);
}

inline std::uint8_t scaleTrig8(std::uint32_t raw, const HidAxis& a) {
    const std::int32_t v = toSigned(raw, a.bitSize, a.logicalMin);
    const std::int32_t span = a.logicalMax - a.logicalMin;
    if (span <= 0) { return 0; }
    auto scaled =
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
// One item from the descriptor stream.
struct HidItem {
    std::uint8_t type = 0;
    std::uint8_t tag = 0;
    std::uint8_t dataLen = 0;
    std::uint32_t data = 0;
    bool skip = false;      // a long item, which no gamepad descriptor uses
    bool truncated = false; // the stream ended mid-item
};

// The global and local item state the stream accumulates until a Main item consumes it.
struct HidParseState {
    std::uint32_t usagePage = 0;
    std::uint32_t reportSize = 0;
    std::uint32_t reportCount = 0;
    std::int32_t logMin = 0;
    std::int32_t logMax = 0;
    std::uint8_t currentReportId = 0;
    std::uint32_t bitCursor = 0;
    bool locked = false;
    std::uint8_t lockedReportId = 0;

    std::uint32_t usages[kMaxUsages] = {};
    std::size_t usageCount = 0;
    std::uint32_t usageMin = 0;
    bool haveRange = false;
};

// A long item carries a payload-length byte, then a tag byte, then that many data bytes. No
// gamepad descriptor uses one, so it is stepped over whole.
inline HidItem readLongItem(const std::uint8_t* desc, std::size_t len, std::size_t& i) {
    HidItem item;
    if (i >= len) {
        item.truncated = true;
        return item;
    }
    const std::uint8_t payload = desc[i];
    i += 2u + payload;
    item.skip = true;
    return item;
}

// A short item packs its data length, type and tag into the prefix byte; bSize 3 means 4 bytes,
// which is the one size that is not its own encoding.
inline HidItem readShortItem(const std::uint8_t* desc, std::size_t len, std::uint8_t prefix,
                             std::size_t& i) {
    HidItem item;
    const std::uint8_t bSize = prefix & 0x03u;
    item.dataLen = bSize == 3 ? 4 : bSize;
    item.type = (prefix >> 2) & 0x03u;
    item.tag = (prefix >> 4) & 0x0Fu;
    if (i + item.dataLen > len) {
        item.truncated = true;
        return item;
    }
    for (std::uint8_t k = 0; k < item.dataLen; k++) {
        item.data |= static_cast<std::uint32_t>(desc[i + k]) << (8u * k);
    }
    i += item.dataLen;
    return item;
}

inline HidItem readHidItem(const std::uint8_t* desc, std::size_t len, std::size_t& i) {
    const std::uint8_t prefix = desc[i++];
    if (prefix == 0xFE) { return readLongItem(desc, len, i); }
    return readShortItem(desc, len, prefix, i);
}

// A report id resets the bit cursor: offsets are relative to the payload of the report they are
// in, not to the descriptor.
inline void applyGlobalItem(const HidItem& item, HidParseState& st) {
    switch (item.tag) {
    case 0x0:
        st.usagePage = item.data;
        break;
    case 0x1:
        st.logMin = signExtend(item.data, item.dataLen);
        break;
    case 0x2:
        st.logMax = signExtend(item.data, item.dataLen);
        break;
    case 0x7:
        st.reportSize = item.data;
        break;
    case 0x8:
        st.currentReportId = static_cast<std::uint8_t>(item.data);
        st.bitCursor = 0;
        break;
    case 0x9:
        st.reportCount = item.data;
        break;
    default:
        break;
    }
}

// A usage minimum implies the range; a usage maximum only confirms it, because the count comes
// from the report count either way.
inline void applyLocalItem(const HidItem& item, HidParseState& st) {
    switch (item.tag) {
    case 0x0:
        if (st.usageCount < kMaxUsages) { st.usages[st.usageCount++] = item.data; }
        break;
    case 0x1:
        st.usageMin = item.data;
        st.haveRange = true;
        break;
    case 0x2:
        st.haveRange = true;
        break;
    default:
        break;
    }
}

// The first button field wins: a descriptor that declares a second one is describing something
// else, and the layout carries one run.
inline void takeButtonField(const HidParseState& st, std::uint32_t startBit, HidLayout& out) {
    if (out.buttonCount != 0) { return; }
    out.buttonBitOffset = static_cast<std::uint16_t>(startBit);
    const std::uint32_t cnt = st.reportCount > kMaxButtons ? kMaxButtons : st.reportCount;
    out.buttonCount = static_cast<std::uint8_t>(cnt);
}

// One usage per field, either counted up from the range's minimum or taken from the declared
// list, whose last entry repeats when the report declares more fields than usages.
inline void takeAxisFields(const HidParseState& st, std::uint32_t startBit, HidLayout& out) {
    for (std::uint32_t f = 0; f < st.reportCount; f++) {
        std::uint32_t usage = 0;
        if (st.haveRange) {
            usage = st.usageMin + f;
        } else if (st.usageCount == 0) {
            break;
        } else {
            usage = st.usages[f < st.usageCount ? f : st.usageCount - 1];
        }
        assignUsage(out, st.usagePage, usage, startBit + f * st.reportSize, st.reportSize,
                    st.logMin, st.logMax);
    }
}

// Constant padding carries no field, and the cursor still has to move past it. The first report
// that carries fields locks the layout: a device with several input reports is described by the
// one this parse can decode, not by a mixture.
inline void applyInputItem(const HidItem& item, HidParseState& st, HidLayout& out) {
    const std::uint32_t startBit = st.bitCursor;
    st.bitCursor += st.reportSize * st.reportCount;

    const bool isConstantPadding = (item.data & 0x01u) != 0;
    const bool carriesFields = st.reportSize > 0 && st.reportCount > 0;
    if (isConstantPadding || !carriesFields) { return; }

    if (!st.locked) {
        st.locked = true;
        st.lockedReportId = st.currentReportId;
        out.reportId = st.currentReportId;
    }
    if (st.currentReportId != st.lockedReportId) { return; }

    if (st.usagePage == 0x09) {
        takeButtonField(st, startBit, out);
        return;
    }
    if (st.usagePage == 0x01 || st.usagePage == 0x02) { takeAxisFields(st, startBit, out); }
}

inline void clearLocalItems(HidParseState& st) {
    st.usageCount = 0;
    st.haveRange = false;
    st.usageMin = 0;
}

// A Main item consumes whatever the Global and Local items have accumulated and then clears the
// Local ones; only an Input main item carries fields this parse wants.
inline void applyItem(const HidItem& item, HidParseState& st, HidLayout& out) {
    if (item.type == 0) {
        if (item.tag == 0x8) { applyInputItem(item, st, out); }
        clearLocalItems(st);
        return;
    }
    if (item.type == 1) {
        applyGlobalItem(item, st);
        return;
    }
    if (item.type == 2) { applyLocalItem(item, st); }
}

inline bool parseReportDescriptor(const std::uint8_t* desc, std::size_t len, HidLayout& out) {
    out = HidLayout{};
    HidParseState st;

    std::size_t i = 0;
    while (i < len) {
        const HidItem item = readHidItem(desc, len, i);
        if (item.truncated) { break; }
        if (item.skip) { continue; }
        applyItem(item, st, out);
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
