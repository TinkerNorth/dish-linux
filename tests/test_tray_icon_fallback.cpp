// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/tray/SniIcon.h"
#include "source/tray/StatusNotifierTrayIcon.h"

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QLatin1String>

#include <cstdint>

using dish::source::kSniIconName;
using dish::source::sniIconName;
using dish::source::sniTrayPixmaps;
using dish::source::StatusNotifierTrayIcon;
using dish::source::toSniPixmap;

TEST_CASE("SniIcon: an unresolvable theme icon advertises no IconName", "[tray]") {
    REQUIRE(sniIconName(false).isEmpty());
}

TEST_CASE("SniIcon: a resolvable theme icon advertises the reverse-DNS name", "[tray]") {
    REQUIRE(sniIconName(true) == QLatin1String(kSniIconName));
}

TEST_CASE("SniIcon: the bundled pixmaps decode at the exact panel sizes", "[tray]") {
    const auto pixmaps = sniTrayPixmaps();
    REQUIRE(pixmaps.size() == 2);
    const int sizes[] = {22, 48};
    for (int i = 0; i < 2; ++i) {
        const auto& pixmap = pixmaps.at(i);
        REQUIRE(pixmap.width == sizes[i]);
        REQUIRE(pixmap.height == sizes[i]);
        REQUIRE(pixmap.data.size() ==
                static_cast<qsizetype>(pixmap.width) * static_cast<qsizetype>(pixmap.height) * 4);
        bool anyInk = false;
        bool anyTransparent = false;
        for (qsizetype offset = 0; offset < pixmap.data.size(); offset += 4) {
            const auto alpha = static_cast<std::uint8_t>(pixmap.data.at(offset));
            if (alpha > 0) { anyInk = true; }
            if (alpha == 0) { anyTransparent = true; }
        }
        REQUIRE(anyInk);
        REQUIRE(anyTransparent);
    }
}

TEST_CASE("SniIcon: pixmap bytes are ARGB in network byte order", "[tray]") {
    QImage image(2, 1, QImage::Format_ARGB32);
    image.setPixel(0, 0, 0x80402010U);
    image.setPixel(1, 0, 0x01020304U);

    const auto pixmap = toSniPixmap(image);
    REQUIRE(pixmap.width == 2);
    REQUIRE(pixmap.height == 1);
    REQUIRE(pixmap.data.size() == 8);

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(pixmap.data.constData());
    REQUIRE(bytes[0] == 0x80);
    REQUIRE(bytes[1] == 0x40);
    REQUIRE(bytes[2] == 0x20);
    REQUIRE(bytes[3] == 0x10);
    REQUIRE(bytes[4] == 0x01);
    REQUIRE(bytes[5] == 0x02);
    REQUIRE(bytes[6] == 0x03);
    REQUIRE(bytes[7] == 0x04);
}

TEST_CASE("StatusNotifierTrayIcon: no IconName before the theme is probed", "[tray]") {
    StatusNotifierTrayIcon icon;
    REQUIRE(icon.iconName().isEmpty());
}

TEST_CASE("StatusNotifierTrayIcon: the tooltip mirrors the item's icon fields", "[tray]") {
    StatusNotifierTrayIcon icon;
    const auto tip = icon.toolTip();
    REQUIRE(tip.iconName == icon.iconName());
    REQUIRE(tip.iconPixmap == icon.iconPixmap());
    REQUIRE(tip.title == QStringLiteral("Dish"));
}
