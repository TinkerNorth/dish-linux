// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Bluetooth-vs-USB classification. Two consumers key off it: the HidrawGateway
// claim filter (a Bluetooth pad decodes garbage through the USB parsers) and
// the SDL bridge's wired/wireless presentation.
//
// The production answer comes from sysfs, which a unit test cannot fabricate,
// so what is pinned here is the parse of the uevent line and the fail-safe
// behaviour for paths that resolve to nothing. A path shape that reaches no
// uevent must read as not-Bluetooth, because the wired presentation is the
// safe default.
//
// The hidraw gateway reads the same uevent, one level further: it enumerates off
// sysfs so a node the user cannot open still shows up, and it needs the vendor
// and product out of the HID_ID triple as well as the bus. That parse is pinned
// here, beside the classifier it shares a format with.

#include "core/input/HidTransport.h"
#include "source/usb/HidrawGateway.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>
#include <string_view>

using dish::input::isBluetoothHidDevicePath;
using dish::input::detail::ueventSaysBluetooth;
using dish::source::usb::detail::parseHidIds;
using dish::source::usb::detail::ueventValue;

namespace {

// One uevent file with the given HID_ID line, returning its path.
std::string writeUevent(const QTemporaryDir& dir, const QString& name, const QString& body) {
    const QString path = dir.filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(body.toUtf8());
    file.close();
    return path.toStdString();
}

} // namespace

TEST_CASE("uevent parse: bus 0005 is Bluetooth, 0003 is USB", "[input][hid-transport]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    const auto usb = writeUevent(dir, QStringLiteral("usb"),
                                 QStringLiteral("DRIVER=hid-generic\n"
                                                "HID_ID=0003:0000054C:00000CE6\n"
                                                "HID_NAME=Wireless Controller\n"));
    CHECK_FALSE(ueventSaysBluetooth(usb));

    const auto bt = writeUevent(dir, QStringLiteral("bt"),
                                QStringLiteral("DRIVER=playstation\n"
                                               "HID_ID=0005:0000054C:00000CE6\n"
                                               "HID_NAME=DualSense Wireless Controller\n"));
    CHECK(ueventSaysBluetooth(bt));
}

TEST_CASE("uevent parse: HID_ID need not be the first line", "[input][hid-transport]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto bt = writeUevent(dir, QStringLiteral("late"),
                                QStringLiteral("DRIVER=playstation\n"
                                               "MODALIAS=hid:b0005g0001v0000054Cp00000CE6\n"
                                               "HID_ID=0005:0000054C:00000CE6\n"));
    CHECK(ueventSaysBluetooth(bt));
}

TEST_CASE("uevent parse: a file with no HID_ID reads as not-Bluetooth", "[input][hid-transport]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto none = writeUevent(dir, QStringLiteral("none"),
                                  QStringLiteral("DRIVER=usbhid\nMODALIAS=usb:v054Cp0CE6\n"));
    CHECK_FALSE(ueventSaysBluetooth(none));
}

TEST_CASE("uevent parse: an unreadable path reads as not-Bluetooth", "[input][hid-transport]") {
    CHECK_FALSE(ueventSaysBluetooth("/nonexistent/dish/uevent"));
}

TEST_CASE("classification fails safe for paths that resolve to nothing", "[input][hid-transport]") {
    CHECK_FALSE(isBluetoothHidDevicePath(""));
    // A node number that exists on no machine: the lookup misses and the wired
    // presentation is the answer.
    CHECK_FALSE(isBluetoothHidDevicePath("/dev/hidraw9999"));
    CHECK_FALSE(isBluetoothHidDevicePath("/dev/input/event9999"));
    // A path shape the classifier does not know is not a guess.
    CHECK_FALSE(isBluetoothHidDevicePath("/dev/ttyUSB0"));
    CHECK_FALSE(isBluetoothHidDevicePath("some-opaque-sdl-identifier"));
}

TEST_CASE("hidraw uevent: the HID_ID triple carries bus, vendor and product",
          "[input][hid-transport]") {
    const auto ids = parseHidIds("DRIVER=hid-generic\n"
                                 "HID_ID=0003:0000054C:00000CE6\n"
                                 "HID_NAME=Wireless Controller\n");
    REQUIRE(ids.has_value());
    CHECK(ids->bus == 0x0003U);
    CHECK(ids->vendorId == 0x054CU);
    CHECK(ids->productId == 0x0CE6U);
}

TEST_CASE("hidraw uevent: a Bluetooth pad reports bus 0005, which the USB-only rule drops",
          "[input][hid-transport]") {
    const auto ids = parseHidIds("DRIVER=playstation\n"
                                 "HID_ID=0005:0000054C:00000CE6\n");
    REQUIRE(ids.has_value());
    CHECK(ids->bus == 0x0005U);
}

TEST_CASE("hidraw uevent: an identity that cannot be read is skipped, never guessed",
          "[input][hid-transport]") {
    // Position in the file does not matter, the same as for the classifier.
    CHECK(parseHidIds("MODALIAS=hid:b0003g0001v0000054Cp00000CE6\n"
                      "HID_ID=0003:0000054C:00000CE6\n")
              .has_value());
    CHECK_FALSE(parseHidIds("DRIVER=usbhid\nMODALIAS=usb:v054Cp0CE6\n").has_value());
    CHECK_FALSE(parseHidIds("").has_value());
    CHECK_FALSE(parseHidIds("HID_ID=0003:0000054C\n").has_value());      // a field short
    CHECK_FALSE(parseHidIds("HID_ID=0003:zzzz:00000CE6\n").has_value()); // not hex
    CHECK_FALSE(parseHidIds("HID_ID=\n").has_value());
    // The key has to match whole, not as a prefix.
    CHECK_FALSE(parseHidIds("HID_IDENT=0003:0000054C:00000CE6\n").has_value());
}

TEST_CASE("hidraw uevent: HID_NAME is the unmodelled pad's fallback name",
          "[input][hid-transport]") {
    // Fallback, not first choice: enumeration prefers the USB `product`
    // attribute (the string the audio stack also names endpoints with) and
    // reads HID_NAME only when that is absent.
    const std::string_view uevent = "DRIVER=playstation\n"
                                    "HID_ID=0003:0000054C:00000CE6\n"
                                    "HID_NAME=Sony Interactive Entertainment DualSense\n"
                                    "HID_PHYS=usb-0000:00:14.0-3/input0\n";
    CHECK(ueventValue(uevent, "HID_NAME") == "Sony Interactive Entertainment DualSense");
    CHECK(ueventValue(uevent, "HID_UNIQ").empty());
    CHECK(ueventValue(uevent, "HID_NA").empty());
    // A uevent whose last line has no trailing newline still reads.
    CHECK(ueventValue("HID_NAME=8BitDo Pro 2", "HID_NAME") == "8BitDo Pro 2");
}
