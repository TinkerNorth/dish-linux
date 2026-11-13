// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The INPUT extended-buttons word the decoders fill, in SDL's names
// (core/model/Protocol.h kExtButton*), restated here like the battery status so
// the decoder headers stay free of the wire protocol; test_usb_report_parsers
// pins it to Protocol.h bit for bit.

#pragma once

#include <cstdint>

namespace dish::input::usbparse::extbtn {

inline constexpr std::uint32_t kRightPaddle1 = 0x0001;
inline constexpr std::uint32_t kLeftPaddle1 = 0x0002;
inline constexpr std::uint32_t kRightPaddle2 = 0x0004;
inline constexpr std::uint32_t kLeftPaddle2 = 0x0008;
inline constexpr std::uint32_t kMisc1 = 0x0010;
inline constexpr std::uint32_t kLeftPadClick = 0x0020;
inline constexpr std::uint32_t kRightPadClick = 0x0040;
inline constexpr std::uint32_t kLeftTriggerClick = 0x0080;
inline constexpr std::uint32_t kRightTriggerClick = 0x0100;
inline constexpr std::uint32_t kLeftStickTouch = 0x0200;
inline constexpr std::uint32_t kRightStickTouch = 0x0400;
inline constexpr std::uint32_t kLeftGripTouch = 0x0800;
inline constexpr std::uint32_t kRightGripTouch = 0x1000;

} // namespace dish::input::usbparse::extbtn
