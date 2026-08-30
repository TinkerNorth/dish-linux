// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/moonlight/MoonlightWire.h"

#include "Util/Endian.h"
#include "core/moonlight/MoonlightProtocol.h"

#include <cstring>

namespace dish::moonwire {
namespace {

// Little-endian writers. The control stream is little-endian except where a
// field is explicitly called out as big-endian (the INPUT_DATA size prefix and
// the mouse deltas).
void putU16Le(std::uint8_t* dst, std::uint16_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
}

void putU32Le(std::uint8_t* dst, std::uint32_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    dst[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
}

void putI16Le(std::uint8_t* dst, std::int16_t v) noexcept {
    putU16Le(dst, static_cast<std::uint16_t>(v));
}

void putF32Le(std::uint8_t* dst, float v) noexcept {
    static_assert(sizeof(float) == 4, "netfloat assumes 32-bit IEEE-754 floats");
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32Le(dst, bits);
}

std::uint16_t readU16Le(const std::uint8_t* src) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(src[0]) |
                                      (static_cast<std::uint16_t>(src[1]) << 8));
}

// Writes the [type][len] control header plus the INPUT_DATA wrapper: the
// data size (BIG-endian, covering input type + body) and the input type
// (little-endian). Returns the offset the body starts at, which is 12.
std::size_t putInputHeader(std::uint8_t* out, std::uint32_t inputType, std::size_t bodyLen) {
    const std::size_t dataSize = 4 + bodyLen; // input type + body
    putU16Le(out, moonproto::kPktInputData);
    putU16Le(out + 2, static_cast<std::uint16_t>(4 + dataSize)); // size prefix + data
    util::putU32Be(out + 4, static_cast<std::uint32_t>(dataSize));
    putU32Le(out + 8, inputType);
    return 12;
}

// CONTROLLER_MULTI's fixed filler words, observed on the wire and named after
// Wolf's CONTROLLER_MULTI_PACKET fields. They are remnants of the legacy
// multi-controller framing and constant in the modern format.
constexpr std::uint16_t kMultiHeaderB = 0x001A;
constexpr std::uint16_t kMultiMidB = 0x0014;
constexpr std::uint16_t kMultiTailA = 0x009C;
constexpr std::uint16_t kMultiTailB = 0x0055;

} // namespace

std::size_t encodeControllerMulti(std::uint8_t* out, std::uint8_t controllerNumber,
                                  std::uint16_t activeMask, std::uint32_t buttonFlags,
                                  std::uint8_t leftTrigger, std::uint8_t rightTrigger,
                                  std::int16_t leftX, std::int16_t leftY, std::int16_t rightX,
                                  std::int16_t rightY) {
    std::size_t off = putInputHeader(out, moonproto::kInputControllerMulti, 26);
    putU16Le(out + off, kMultiHeaderB);
    putU16Le(out + off + 2, static_cast<std::uint16_t>(controllerNumber));
    putU16Le(out + off + 4, activeMask);
    putU16Le(out + off + 6, kMultiMidB);
    putU16Le(out + off + 8, static_cast<std::uint16_t>(buttonFlags & 0xFFFFU));
    out[off + 10] = leftTrigger;
    out[off + 11] = rightTrigger;
    putI16Le(out + off + 12, leftX);
    putI16Le(out + off + 14, leftY);
    putI16Le(out + off + 16, rightX);
    putI16Le(out + off + 18, rightY);
    putU16Le(out + off + 20, kMultiTailA);
    putU16Le(out + off + 22, static_cast<std::uint16_t>((buttonFlags >> 16) & 0xFFFFU));
    putU16Le(out + off + 24, kMultiTailB);
    return kControllerMultiSize;
}

std::size_t encodeControllerArrival(std::uint8_t* out, std::uint8_t controllerNumber,
                                    std::uint8_t controllerType, std::uint8_t capabilities,
                                    std::uint32_t supportedButtons) {
    std::size_t off = putInputHeader(out, moonproto::kInputControllerArrival, 8);
    out[off] = controllerNumber;
    out[off + 1] = controllerType;
    out[off + 2] = capabilities;
    out[off + 3] = 0;
    putU32Le(out + off + 4, supportedButtons);
    return kControllerArrivalSize;
}

std::size_t encodeControllerMotion(std::uint8_t* out, std::uint8_t controllerNumber,
                                   std::uint8_t motionType, float x, float y, float z) {
    std::size_t off = putInputHeader(out, moonproto::kInputControllerMotion, 16);
    out[off] = controllerNumber;
    out[off + 1] = motionType;
    out[off + 2] = 0;
    out[off + 3] = 0;
    putF32Le(out + off + 4, x);
    putF32Le(out + off + 8, y);
    putF32Le(out + off + 12, z);
    return kControllerMotionSize;
}

std::size_t encodeControllerBattery(std::uint8_t* out, std::uint8_t controllerNumber,
                                    std::uint8_t state, std::uint8_t percentage) {
    std::size_t off = putInputHeader(out, moonproto::kInputControllerBattery, 4);
    out[off] = controllerNumber;
    out[off + 1] = state;
    out[off + 2] = percentage;
    out[off + 3] = 0;
    return kControllerBatterySize;
}

std::size_t encodeMouseMoveRel(std::uint8_t* out, std::int16_t deltaX, std::int16_t deltaY) {
    std::size_t off = putInputHeader(out, moonproto::kInputMouseMoveRel, 4);
    util::putU16Be(out + off, static_cast<std::uint16_t>(deltaX));
    util::putU16Be(out + off + 2, static_cast<std::uint16_t>(deltaY));
    return kMouseMoveRelSize;
}

std::size_t encodePeriodicPing(std::uint8_t* out) {
    // Byte-for-byte the keep-alive a real client sends (Wolf testControl.cpp's
    // captured session): type 0x0200, len 8, then the fixed payload.
    static constexpr std::uint8_t kPing[kPeriodicPingSize] = {0x00, 0x02, 0x08, 0x00, 0x04, 0x00,
                                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(out, kPing, sizeof(kPing));
    return kPeriodicPingSize;
}

std::size_t encodeTermination(std::uint8_t* out) {
    putU16Le(out, moonproto::kPktTermination);
    putU16Le(out + 2, 4);
    // The reason rides big-endian, per Wolf's TERMINATE_REASON_GRACEFULL.
    util::putU32Be(out + 4, moonproto::kTerminateReasonGraceful);
    return kTerminationSize;
}

std::size_t encodeRtpPing(std::uint8_t* out, const char* payload, std::size_t payloadLen,
                          std::uint32_t sequence) {
    if (payload == nullptr || payloadLen == 0) {
        // Legacy 4-byte ping, for hosts that never advertised a payload.
        out[0] = 'P';
        out[1] = 'I';
        out[2] = 'N';
        out[3] = 'G';
        return kRtpPingLegacySize;
    }
    // SS_PING: the payload field is a fixed 16 bytes on the wire, so a short
    // header value is zero-padded and an overlong one truncated.
    constexpr std::size_t kPayloadField = 16;
    std::memset(out, 0, kPayloadField);
    std::memcpy(out, payload, payloadLen < kPayloadField ? payloadLen : kPayloadField);
    putU32Le(out + kPayloadField, sequence);
    return kRtpPingSize;
}

std::optional<HostEvent> decodeHostEvent(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 4) { return std::nullopt; }
    const std::uint16_t type = readU16Le(data);
    const std::uint8_t* body = data + 4;
    const std::size_t bodyLen = len - 4;

    HostEvent ev;
    switch (type) {
    case moonproto::kPktRumbleData: {
        // [unused u32][ctrl u16][low u16][high u16]
        if (bodyLen < 10) { return std::nullopt; }
        ev.type = HostEventType::Rumble;
        ev.controllerNumber = readU16Le(body + 4);
        ev.rumbleLow = readU16Le(body + 6);
        ev.rumbleHigh = readU16Le(body + 8);
        return ev;
    }
    case moonproto::kPktRumbleTriggers: {
        // [ctrl u16][left u16][right u16]
        if (bodyLen < 6) { return std::nullopt; }
        ev.type = HostEventType::RumbleTriggers;
        ev.controllerNumber = readU16Le(body);
        ev.rumbleLow = readU16Le(body + 2);
        ev.rumbleHigh = readU16Le(body + 4);
        return ev;
    }
    case moonproto::kPktMotionEvent: {
        // [ctrl u16][rate u16][type u8]
        if (bodyLen < 5) { return std::nullopt; }
        ev.type = HostEventType::MotionRequest;
        ev.controllerNumber = readU16Le(body);
        ev.motionRateHz = readU16Le(body + 2);
        ev.motionType = body[4];
        return ev;
    }
    case moonproto::kPktRgbLed: {
        // [ctrl u16][r][g][b]
        if (bodyLen < 5) { return std::nullopt; }
        ev.type = HostEventType::RgbLed;
        ev.controllerNumber = readU16Le(body);
        ev.red = body[2];
        ev.green = body[3];
        ev.blue = body[4];
        return ev;
    }
    case moonproto::kPktTermination: {
        ev.type = HostEventType::Termination;
        return ev;
    }
    default:
        ev.type = HostEventType::Unknown;
        return ev;
    }
}

} // namespace dish::moonwire
