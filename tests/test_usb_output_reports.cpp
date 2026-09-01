// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Byte-exact pins for the OUT reports a Direct-claimed pad accepts. These bytes
// reach firmware, so nothing here is asserted loosely: every offset that carries
// a valid-flag, a magnitude or a colour is checked by index, and every byte that
// must stay zero is checked to be zero.
//
// The layouts are the ones SDL's HIDAPI drivers and the kernel's hid-playstation
// / hid-nintendo write, and the ones dish-android already ships against real
// hardware. A test that merely re-states the implementation would be worthless,
// so the cases below are written from the layout, not from the code.

#include "core/input/UsbOutputReports.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using dish::input::usbout::buildLightbarReport;
using dish::input::usbout::buildPlayerLedsReport;
using dish::input::usbout::buildRumbleReport;
using dish::input::usbout::buildTriggerEffectsReport;
using dish::input::usbout::FeedbackState;
using dish::input::usbout::kMaxOutputReportBytes;
using dish::input::usbout::kTriggerEffectBlockBytes;
using dish::input::usbout::parserHasLightbar;
using dish::input::usbout::parserHasPlayerLeds;
using dish::input::usbout::parserHasTriggerEffects;
using dish::input::usbparse::HidParser;

namespace {

using Buf = std::array<std::uint8_t, kMaxOutputReportBytes>;

// Every byte outside the named set must be zero: a stray non-zero byte in a
// DualSense output report is another valid-flag or another actuator.
void checkZeroExcept(const Buf& buf, std::size_t len, const std::vector<std::size_t>& allowed) {
    for (std::size_t i = 0; i < len; ++i) {
        bool skip = false;
        for (const auto a : allowed) {
            if (a == i) {
                skip = true;
                break;
            }
        }
        if (skip) { continue; }
        INFO("byte " << i << " should be zero");
        CHECK(buf[i] == 0);
    }
}

} // namespace

TEST_CASE("only the families with the hardware carry each actuator", "[usb][output]") {
    // The descriptor's caps are built from exactly these, so a wrong answer here
    // makes the satellite send a message that lands nowhere.
    CHECK(parserHasLightbar(HidParser::DualShock4));
    CHECK(parserHasLightbar(HidParser::DualSense));
    CHECK_FALSE(parserHasLightbar(HidParser::SwitchProUsb));
    CHECK_FALSE(parserHasLightbar(HidParser::GenericHid));
    CHECK_FALSE(parserHasLightbar(HidParser::SteamController));
    CHECK_FALSE(parserHasLightbar(HidParser::None));

    // The Switch Pro's four grip lights are player LEDs; it has no RGB bar.
    CHECK(parserHasPlayerLeds(HidParser::DualSense));
    CHECK(parserHasPlayerLeds(HidParser::SwitchProUsb));
    CHECK_FALSE(parserHasPlayerLeds(HidParser::DualShock4));
    CHECK_FALSE(parserHasPlayerLeds(HidParser::GenericHid));

    // Adaptive triggers are a DualSense exclusive.
    CHECK(parserHasTriggerEffects(HidParser::DualSense));
    CHECK_FALSE(parserHasTriggerEffects(HidParser::DualShock4));
    CHECK_FALSE(parserHasTriggerEffects(HidParser::SwitchProUsb));
}

TEST_CASE("DualShock 4 rumble is report 0x05 with the motors flagged valid", "[usb][output]") {
    Buf buf{};
    const std::size_t n =
        buildRumbleReport(HidParser::DualShock4, 0xAB00, 0xCD00, /*seq=*/0, buf.data(), buf.size());
    REQUIRE(n == 32);
    CHECK(buf[0] == 0x05); // report id
    CHECK(buf[1] == 0x01); // valid flags: motors only, so a colour is untouched
    // Weak first, then strong: the DS4 orders the motors the opposite way round
    // from the argument list, which is exactly the swap worth pinning.
    CHECK(buf[4] == 0xCD);
    CHECK(buf[5] == 0xAB);
    checkZeroExcept(buf, n, {0, 1, 4, 5});
}

TEST_CASE("DualSense rumble is report 0x02 with COMPATIBLE_VIBRATION", "[usb][output]") {
    Buf buf{};
    const std::size_t n =
        buildRumbleReport(HidParser::DualSense, 0x1200, 0x3400, /*seq=*/0, buf.data(), buf.size());
    REQUIRE(n == 63);
    CHECK(buf[0] == 0x02);
    CHECK(buf[1] == 0x01); // valid_flag0
    CHECK(buf[3] == 0x34); // weak
    CHECK(buf[4] == 0x12); // strong
    checkZeroExcept(buf, n, {0, 1, 3, 4});
}

TEST_CASE("only the top byte of a magnitude reaches an 8-bit motor", "[usb][output]") {
    // A caller passing 0x00FF must not produce a motor at 0xFF: the pads take
    // the high byte, so sub-256 magnitudes are silence, not full power.
    Buf buf{};
    const std::size_t n =
        buildRumbleReport(HidParser::DualSense, 0x00FF, 0x00FF, 0, buf.data(), buf.size());
    REQUIRE(n == 63);
    CHECK(buf[3] == 0);
    CHECK(buf[4] == 0);
}

TEST_CASE("Switch Pro rumble encodes both motors and advances the packet counter",
          "[usb][output]") {
    Buf buf{};
    const std::size_t n =
        buildRumbleReport(HidParser::SwitchProUsb, 0xFFFF, 0, /*seq=*/0x35, buf.data(), buf.size());
    REQUIRE(n == 10);
    CHECK(buf[0] == 0x10);        // rumble-only report
    CHECK(buf[1] == 0x05);        // seq masked to the low nibble
    CHECK(buf[2] == 0x00);        // the encoded pair, not the raw magnitude
    CHECK(buf[3] == 0x01 + 0xC8); // full amplitude -> the top code
    CHECK(buf[4] == 0x40 + 0x00);
    CHECK(buf[5] == 0x72);
    // A zero magnitude lands exactly on the neutral code, so "stop" is a real
    // stop rather than the quietest buzz.
    CHECK(buf[6] == 0x00);
    CHECK(buf[7] == 0x01);
    CHECK(buf[8] == 0x40);
    CHECK(buf[9] == 0x40);
}

TEST_CASE("Switch Pro amplitude rounds down between table steps", "[usb][output]") {
    // Halfway is amp 501, which sits between the 387 and 650 codes. Rounding UP
    // would make a gentle rumble jump a step.
    Buf buf{};
    REQUIRE(buildRumbleReport(HidParser::SwitchProUsb, 0x8000, 0, 0, buf.data(), buf.size()) == 10);
    CHECK(buf[3] == 0x01 + 0x70);
    CHECK(buf[4] == 0x40 + 0x00);
    CHECK(buf[5] == 0x5C);
}

TEST_CASE("families without motors build nothing rather than a wrong report", "[usb][output]") {
    Buf buf{};
    CHECK(buildRumbleReport(HidParser::SteamController, 0xFFFF, 0xFFFF, 0, buf.data(),
                            buf.size()) == 0);
    CHECK(buildRumbleReport(HidParser::GenericHid, 0xFFFF, 0xFFFF, 0, buf.data(), buf.size()) == 0);
    CHECK(buildRumbleReport(HidParser::None, 0xFFFF, 0xFFFF, 0, buf.data(), buf.size()) == 0);
}

TEST_CASE("a buffer too small builds nothing rather than a partial report", "[usb][output]") {
    // The guard is what keeps a short buffer from becoming a half-written report
    // on the wire; every family gets its own check because each has its own size.
    std::array<std::uint8_t, 9> tiny{};
    FeedbackState st;
    CHECK(buildRumbleReport(HidParser::DualShock4, 1, 1, 0, tiny.data(), tiny.size()) == 0);
    CHECK(buildRumbleReport(HidParser::DualSense, 1, 1, 0, tiny.data(), tiny.size()) == 0);
    CHECK(buildRumbleReport(HidParser::SwitchProUsb, 1, 1, 0, tiny.data(), tiny.size()) == 0);
    CHECK(buildLightbarReport(HidParser::DualShock4, st, 1, 2, 3, tiny.data(), tiny.size()) == 0);
    CHECK(buildLightbarReport(HidParser::DualSense, st, 1, 2, 3, tiny.data(), tiny.size()) == 0);
    CHECK(buildPlayerLedsReport(HidParser::DualSense, 0x01, 0, tiny.data(), tiny.size()) == 0);
    CHECK(buildPlayerLedsReport(HidParser::SwitchProUsb, 0x01, 0, tiny.data(), tiny.size()) == 0);
    const std::array<std::uint8_t, kTriggerEffectBlockBytes> block{};
    CHECK(buildTriggerEffectsReport(HidParser::DualSense, block.data(), block.data(), tiny.data(),
                                    tiny.size()) == 0);
    // ...and the setup flag must NOT have been consumed by a build that failed.
    CHECK_FALSE(st.ds5LightbarSetupSent);
}

TEST_CASE("DualShock 4 colour marks the motors invalid", "[usb][output]") {
    // Flag 0x02 alone. Setting 0x01 too would zero a rumble that is still
    // running every time the game changed the colour.
    Buf buf{};
    FeedbackState st;
    const std::size_t n =
        buildLightbarReport(HidParser::DualShock4, st, 0x11, 0x22, 0x33, buf.data(), buf.size());
    REQUIRE(n == 32);
    CHECK(buf[0] == 0x05);
    CHECK(buf[1] == 0x02);
    CHECK(buf[6] == 0x11);
    CHECK(buf[7] == 0x22);
    CHECK(buf[8] == 0x33);
    checkZeroExcept(buf, n, {0, 1, 6, 7, 8});
}

TEST_CASE("the DualSense's first colour carries the lightbar handoff", "[usb][output]") {
    // Without it the firmware keeps its own blue glow and the host colour never
    // appears at all, which reads as "the lightbar feature does not work".
    Buf buf{};
    FeedbackState st;
    REQUIRE_FALSE(st.ds5LightbarSetupSent);
    const std::size_t n =
        buildLightbarReport(HidParser::DualSense, st, 0x44, 0x55, 0x66, buf.data(), buf.size());
    REQUIRE(n == 63);
    CHECK(buf[0] == 0x02);
    CHECK(buf[2] == 0x04);  // valid_flag1 LIGHTBAR_CONTROL_ENABLE
    CHECK(buf[39] == 0x02); // valid_flag2 LIGHTBAR_SETUP_CONTROL_ENABLE
    CHECK(buf[42] == 0x02); // lightbar_setup = LIGHT_OUT
    CHECK(buf[45] == 0x44);
    CHECK(buf[46] == 0x55);
    CHECK(buf[47] == 0x66);
    checkZeroExcept(buf, n, {0, 2, 39, 42, 45, 46, 47});
    CHECK(st.ds5LightbarSetupSent);
}

TEST_CASE("later DualSense colours drop the handoff", "[usb][output]") {
    // Re-sending it on every colour would re-run the firmware's light-out
    // sequence at whatever rate the game changes colours.
    Buf buf{};
    FeedbackState st;
    REQUIRE(buildLightbarReport(HidParser::DualSense, st, 1, 2, 3, buf.data(), buf.size()) == 63);
    Buf second{};
    const std::size_t n = buildLightbarReport(HidParser::DualSense, st, 0x77, 0x88, 0x99,
                                              second.data(), second.size());
    REQUIRE(n == 63);
    CHECK(second[39] == 0);
    CHECK(second[42] == 0);
    CHECK(second[45] == 0x77);
    CHECK(second[46] == 0x88);
    CHECK(second[47] == 0x99);
    checkZeroExcept(second, n, {0, 2, 45, 46, 47});
}

TEST_CASE("a fresh claim re-sends the handoff", "[usb][output]") {
    // A replugged pad has forgotten it, so the state has to be per-claim rather
    // than per-process. The manager erases the entry on release; this pins that
    // a default-constructed state is the un-sent one.
    FeedbackState fresh;
    CHECK_FALSE(fresh.ds5LightbarSetupSent);
}

TEST_CASE("player LEDs are masked to the LEDs a family actually has", "[usb][output]") {
    // A high bit outside the real LEDs is a reserved firmware flag on both
    // families, so passing 0xFF straight through would be setting flags at
    // random.
    Buf ds{};
    const std::size_t dsLen =
        buildPlayerLedsReport(HidParser::DualSense, 0xFF, /*seq=*/0, ds.data(), ds.size());
    REQUIRE(dsLen == 63);
    CHECK(ds[0] == 0x02);
    CHECK(ds[2] == 0x10);  // valid_flag1 PLAYER_INDICATOR_CONTROL_ENABLE
    CHECK(ds[44] == 0x1F); // five LEDs
    checkZeroExcept(ds, dsLen, {0, 2, 44});

    Buf sw{};
    const std::size_t swLen =
        buildPlayerLedsReport(HidParser::SwitchProUsb, 0xFF, /*seq=*/0x21, sw.data(), sw.size());
    REQUIRE(swLen == 12);
    CHECK(sw[0] == 0x01);  // rumble + subcommand report
    CHECK(sw[1] == 0x01);  // seq low nibble
    CHECK(sw[10] == 0x30); // set player lights
    CHECK(sw[11] == 0x0F); // four lights
}

TEST_CASE("Switch player lights ride neutral rumble blocks", "[usb][output]") {
    // The subcommand shares its report with rumble, so the motor bytes have to
    // be the neutral code or setting a player number would stop a live rumble.
    Buf buf{};
    REQUIRE(buildPlayerLedsReport(HidParser::SwitchProUsb, 0x01, 0, buf.data(), buf.size()) == 12);
    CHECK(buf[2] == 0x00);
    CHECK(buf[3] == 0x01);
    CHECK(buf[4] == 0x40);
    CHECK(buf[5] == 0x40);
    CHECK(buf[6] == 0x00);
    CHECK(buf[7] == 0x01);
    CHECK(buf[8] == 0x40);
    CHECK(buf[9] == 0x40);
}

TEST_CASE("trigger-effect blocks are copied verbatim, right block first", "[usb][output]") {
    // The wire order is (left, right) and the report order is (right, left).
    // Getting this backwards puts the game's left-trigger effect on the right
    // trigger, which is a bug no test of "did we write 63 bytes" would catch.
    std::array<std::uint8_t, kTriggerEffectBlockBytes> left{};
    std::array<std::uint8_t, kTriggerEffectBlockBytes> right{};
    for (std::size_t i = 0; i < kTriggerEffectBlockBytes; ++i) {
        left[i] = static_cast<std::uint8_t>(0xA0 + i);
        right[i] = static_cast<std::uint8_t>(0xB0 + i);
    }
    Buf buf{};
    const std::size_t n = buildTriggerEffectsReport(HidParser::DualSense, left.data(), right.data(),
                                                    buf.data(), buf.size());
    REQUIRE(n == 63);
    CHECK(buf[0] == 0x02);
    CHECK(buf[1] == 0x0C); // valid_flag0: right (0x04) + left (0x08)
    for (std::size_t i = 0; i < kTriggerEffectBlockBytes; ++i) {
        INFO("block byte " << i);
        CHECK(buf[11 + i] == right[i]);
        CHECK(buf[22 + i] == left[i]);
    }
    // Nothing else is touched: an effect write must not disturb the motors, the
    // colour or the player LEDs.
    for (std::size_t i = 2; i < 11; ++i) { CHECK(buf[i] == 0); }
    for (std::size_t i = 33; i < n; ++i) { CHECK(buf[i] == 0); }
}

TEST_CASE("trigger effects build nothing for a family without adaptive triggers", "[usb][output]") {
    const std::array<std::uint8_t, kTriggerEffectBlockBytes> block{};
    Buf buf{};
    CHECK(buildTriggerEffectsReport(HidParser::DualShock4, block.data(), block.data(), buf.data(),
                                    buf.size()) == 0);
    CHECK(buildTriggerEffectsReport(HidParser::SwitchProUsb, block.data(), block.data(), buf.data(),
                                    buf.size()) == 0);
    CHECK(buildTriggerEffectsReport(HidParser::None, block.data(), block.data(), buf.data(),
                                    buf.size()) == 0);
}

TEST_CASE("lightbar and player LEDs build nothing for the wrong family", "[usb][output]") {
    Buf buf{};
    FeedbackState st;
    CHECK(buildLightbarReport(HidParser::SwitchProUsb, st, 1, 2, 3, buf.data(), buf.size()) == 0);
    CHECK(buildLightbarReport(HidParser::None, st, 1, 2, 3, buf.data(), buf.size()) == 0);
    CHECK(buildPlayerLedsReport(HidParser::DualShock4, 1, 0, buf.data(), buf.size()) == 0);
    CHECK(buildPlayerLedsReport(HidParser::GenericHid, 1, 0, buf.data(), buf.size()) == 0);
}

TEST_CASE("the scratch size fits every report a family can produce", "[usb][output]") {
    // kMaxOutputReportBytes is what every caller sizes its buffer with, so a new
    // longer report has to raise it or every build would silently return 0.
    Buf buf{};
    FeedbackState st;
    const std::array<std::uint8_t, kTriggerEffectBlockBytes> block{};
    CHECK(buildRumbleReport(HidParser::DualSense, 1, 1, 0, buf.data(), buf.size()) != 0);
    CHECK(buildLightbarReport(HidParser::DualSense, st, 1, 1, 1, buf.data(), buf.size()) != 0);
    CHECK(buildPlayerLedsReport(HidParser::DualSense, 1, 0, buf.data(), buf.size()) != 0);
    CHECK(buildTriggerEffectsReport(HidParser::DualSense, block.data(), block.data(), buf.data(),
                                    buf.size()) != 0);
}
