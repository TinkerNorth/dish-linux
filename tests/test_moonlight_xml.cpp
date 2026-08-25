// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Documents shaped exactly like the host response builders emit them (Wolf's
// moonlight.cpp; Sunshine and Apollo produce the same fields).

#include "core/moonlight/MoonlightXml.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace dish::moonxml;

namespace {

const std::string kServerInfo =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<root status_code=\"200\">"
    "<hostname>wolfpad</hostname>"
    "<appversion>7.1.431.-1</appversion>"
    "<GfeVersion>3.23.0.74</GfeVersion>"
    "<uniqueid>0f83dfaa-a462-4f22-bbb1-8a6a3fd1b6ba</uniqueid>"
    "<MaxLumaPixelsHEVC>1869449984</MaxLumaPixelsHEVC>"
    "<ServerCodecModeSupport>257</ServerCodecModeSupport>"
    "<HttpsPort>47984</HttpsPort>"
    "<ExternalPort>47989</ExternalPort>"
    "<mac>00:00:00:00:00:00</mac>"
    "<LocalIP>192.168.1.100</LocalIP>"
    "<SupportedDisplayMode><DisplayMode><Width>1920</Width><Height>1080</Height>"
    "<RefreshRate>60</RefreshRate></DisplayMode></SupportedDisplayMode>"
    "<PairStatus>1</PairStatus>"
    "<currentgame>0</currentgame>"
    "<state>SUNSHINE_SERVER_FREE</state>"
    "</root>";

} // namespace

TEST_CASE("parseServerInfo reads the fields the client needs", "[moonlight][xml]") {
    const auto info = parseServerInfo(kServerInfo);
    REQUIRE(info.has_value());
    CHECK(info->hostname == "wolfpad");
    CHECK(info->uuid == "0f83dfaa-a462-4f22-bbb1-8a6a3fd1b6ba");
    CHECK(info->appVersion == "7.1.431.-1");
    CHECK(info->httpsPort == 47984);
    CHECK(info->externalPort == 47989);
    CHECK(info->pairStatus == 1);
    CHECK(info->currentGame == 0);
    CHECK_FALSE(info->busy());
}

TEST_CASE("parseServerInfo flags a busy host", "[moonlight][xml]") {
    std::string busy = kServerInfo;
    const auto at = busy.find("SUNSHINE_SERVER_FREE");
    busy.replace(at, std::string("SUNSHINE_SERVER_FREE").size(), "SUNSHINE_SERVER_BUSY");
    const auto info = parseServerInfo(busy);
    REQUIRE(info.has_value());
    CHECK(info->busy());
}

TEST_CASE("parseServerInfo rejects failure and garbage", "[moonlight][xml]") {
    CHECK_FALSE(parseServerInfo("").has_value());
    CHECK_FALSE(
        parseServerInfo("<root status_code=\"404\"><hostname>x</hostname></root>").has_value());
    CHECK_FALSE(parseServerInfo("<root status_code=\"200\"></root>").has_value());
    CHECK_FALSE(parseServerInfo("not xml at all").has_value());
}

TEST_CASE("parseAppList returns every App row", "[moonlight][xml]") {
    const std::string xml =
        "<root status_code=\"200\">"
        "<App><IsHdrSupported>0</IsHdrSupported><AppTitle>Desktop</AppTitle><ID>1</ID></App>"
        "<App><IsHdrSupported>1</IsHdrSupported><AppTitle>Steam Big Picture</AppTitle>"
        "<ID>2</ID></App>"
        "<App><AppTitle>Fish &amp; Chips</AppTitle><ID>3</ID></App>"
        "</root>";
    const auto apps = parseAppList(xml);
    REQUIRE(apps.size() == 3);
    CHECK(apps[0].title == "Desktop");
    CHECK(apps[0].id == "1");
    CHECK(apps[1].title == "Steam Big Picture");
    CHECK(apps[1].id == "2");
    CHECK(apps[2].title == "Fish & Chips"); // entity decoded
    CHECK(apps[2].id == "3");
}

TEST_CASE("parseAppList tolerates malformed documents", "[moonlight][xml]") {
    CHECK(parseAppList("").empty());
    CHECK(parseAppList("<root status_code=\"200\"></root>").empty());
    CHECK(parseAppList("<root status_code=\"400\"><App><AppTitle>x</AppTitle><ID>9</ID></App>"
                       "</root>")
              .empty());
    // A truncated App block is skipped rather than crashing the parse.
    CHECK(parseAppList("<root status_code=\"200\"><App><AppTitle>x</AppTitle>").empty());
}

TEST_CASE("parseLaunch extracts the RTSP endpoint", "[moonlight][xml]") {
    const auto launch = parseLaunch("<root status_code=\"200\">"
                                    "<sessionUrl0>rtsp://170.55.71.212:48010</sessionUrl0>"
                                    "<gamesession>1</gamesession></root>");
    REQUIRE(launch.has_value());
    CHECK(launch->rtspHost == "170.55.71.212");
    CHECK(launch->rtspPort == 48010);
    CHECK(launch->launched);
}

TEST_CASE("parseLaunch accepts a resume response", "[moonlight][xml]") {
    const auto launch = parseLaunch("<root status_code=\"200\">"
                                    "<sessionUrl0>rtsp://10.0.0.2:31000</sessionUrl0>"
                                    "<resume>1</resume></root>");
    REQUIRE(launch.has_value());
    CHECK(launch->rtspPort == 31000);
    CHECK(launch->launched);
}

TEST_CASE("parseLaunch rejects a missing or malformed session url", "[moonlight][xml]") {
    CHECK_FALSE(
        parseLaunch("<root status_code=\"200\"><gamesession>1</gamesession></root>").has_value());
    CHECK_FALSE(parseLaunch("<root status_code=\"503\">"
                            "<sessionUrl0>rtsp://1.2.3.4:48010</sessionUrl0></root>")
                    .has_value());
    CHECK_FALSE(parseLaunch("<root status_code=\"200\">"
                            "<sessionUrl0>rtsp://1.2.3.4:notaport</sessionUrl0></root>")
                    .has_value());
}

TEST_CASE("pairedFlag and tag helpers", "[moonlight][xml]") {
    CHECK(pairedFlag("<root status_code=\"200\"><paired>1</paired></root>"));
    CHECK_FALSE(pairedFlag("<root status_code=\"200\"><paired>0</paired></root>"));
    CHECK_FALSE(pairedFlag("<root status_code=\"200\"></root>"));

    CHECK(tagValue("<root><plaincert>4142</plaincert></root>", "plaincert") == "4142");
    CHECK_FALSE(tagValue("<root></root>", "plaincert").has_value());
    CHECK(tagInt("<root><currentgame>-1</currentgame></root>", "currentgame") == -1);
    CHECK_FALSE(tagInt("<root><currentgame>abc</currentgame></root>", "currentgame").has_value());
    CHECK(statusCode("<root status_code=\"200\"/>") == 200);
    CHECK_FALSE(statusCode("<root/>").has_value());
}

TEST_CASE("tagValue does not match tags sharing a prefix", "[moonlight][xml]") {
    const std::string xml = "<root><AppTitle>title</AppTitle><App>real</App></root>";
    CHECK(tagValue(xml, "App") == "real");
}
