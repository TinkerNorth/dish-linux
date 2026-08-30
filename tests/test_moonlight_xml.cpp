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

TEST_CASE("a host refuses in the BODY, not in the status line", "[moonlight][xml]") {
    // Measured against a live Sunshine host: asking /launch to start a second
    // app answers HTTP 200 with status_code="400" and "An app is already
    // running on this host". Code that reads only the HTTP status treats that
    // refusal as a success and then fails downstream on the missing
    // sessionUrl0, naming the wrong thing.
    const std::string busy =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<root status_code=\"400\" status_message=\"An app is already running on this host\">"
        "<resume>0</resume></root>";
    const auto status = parseStatus(busy);
    REQUIRE(status.has_value());
    CHECK(status->code == 400);
    CHECK(status->message == "An app is already running on this host");
    CHECK_FALSE(status->ok());
    CHECK_FALSE(status->resume);
    CHECK(status->appAlreadyRunning());
    // And the launch parse refuses it rather than reporting a session.
    CHECK_FALSE(parseLaunch(busy).has_value());
}

TEST_CASE("a resumable refusal carries the resume flag", "[moonlight][xml]") {
    const auto status =
        parseStatus("<root status_code=\"400\" status_message=\"An app is already running on this "
                    "host\"><resume>1</resume></root>");
    REQUIRE(status.has_value());
    CHECK(status->appAlreadyRunning());
    CHECK(status->resume);
}

TEST_CASE("parseStatus reads every endpoint's root element", "[moonlight][xml]") {
    SECTION("a plain success names no status_code at all") {
        const auto status = parseStatus("<root><App><ID>1</ID></App></root>");
        REQUIRE(status.has_value());
        CHECK(status->code == 200);
        CHECK(status->ok());
        CHECK(status->message.empty());
        CHECK_FALSE(status->appAlreadyRunning());
    }
    SECTION("2xx is success, not just 200") {
        const auto status = parseStatus("<root status_code=\"204\"></root>");
        REQUIRE(status.has_value());
        CHECK(status->ok());
    }
    SECTION("a refusal that is not the busy one") {
        const auto status =
            parseStatus("<root status_code=\"401\" status_message=\"Not paired\"></root>");
        REQUIRE(status.has_value());
        CHECK_FALSE(status->ok());
        CHECK(status->message == "Not paired");
        CHECK_FALSE(status->appAlreadyRunning());
    }
    SECTION("the message match is case-insensitive and entity-decoded") {
        const auto status = parseStatus(
            "<root status_code=\"400\" status_message=\"An App Is ALREADY RUNNING &amp; busy\"/>");
        REQUIRE(status.has_value());
        CHECK(status->message == "An App Is ALREADY RUNNING & busy");
        CHECK(status->appAlreadyRunning());
    }
    SECTION("a 2xx that mentions the phrase is still not a refusal") {
        const auto status =
            parseStatus("<root status_code=\"200\" status_message=\"already running\"/>");
        REQUIRE(status.has_value());
        CHECK_FALSE(status->appAlreadyRunning());
    }
    SECTION("no root element at all") { CHECK_FALSE(parseStatus("not xml at all").has_value()); }
}

TEST_CASE("statusMessage reads the root attribute", "[moonlight][xml]") {
    CHECK(statusMessage("<root status_code=\"400\" status_message=\"nope\"/>") == "nope");
    CHECK_FALSE(statusMessage("<root status_code=\"400\"/>").has_value());
}

TEST_CASE("parseServerInfo reads the advertised display modes", "[moonlight][xml]") {
    const auto info = parseServerInfo(kServerInfo);
    REQUIRE(info.has_value());
    REQUIRE(info->displayModes.size() == 1);
    CHECK(info->displayModes[0].width == 1920);
    CHECK(info->displayModes[0].height == 1080);
    CHECK(info->displayModes[0].refreshRate == 60);
}

TEST_CASE("preferredDisplayMode picks the host's own display", "[moonlight][xml]") {
    // Ask for a mode that matches what the host is already showing: an
    // Apollo/Vibepollo virtual display follows the client's request, so asking
    // for something small resizes the user's desktop under them.
    const std::string xml = "<root status_code=\"200\"><hostname>h</hostname>"
                            "<SupportedDisplayMode>"
                            "<DisplayMode><Width>1280</Width><Height>720</Height>"
                            "<RefreshRate>60</RefreshRate></DisplayMode>"
                            "<DisplayMode><Width>2560</Width><Height>1440</Height>"
                            "<RefreshRate>60</RefreshRate></DisplayMode>"
                            "<DisplayMode><Width>2560</Width><Height>1440</Height>"
                            "<RefreshRate>144</RefreshRate></DisplayMode>"
                            "<DisplayMode><Width>1920</Width><Height>1080</Height>"
                            "<RefreshRate>240</RefreshRate></DisplayMode>"
                            "</SupportedDisplayMode></root>";
    const auto info = parseServerInfo(xml);
    REQUIRE(info.has_value());
    CHECK(info->displayModes.size() == 4);
    const auto best = preferredDisplayMode(info->displayModes);
    REQUIRE(best.has_value());
    CHECK(best->width == 2560);
    CHECK(best->height == 1440);
    CHECK(best->refreshRate == 144); // largest area first, then the fastest at it

    // A host that advertises none leaves the caller on its own default.
    CHECK_FALSE(preferredDisplayMode({}).has_value());
    const auto noModes = parseServerInfo("<root status_code=\"200\"><hostname>h</hostname></root>");
    REQUIRE(noModes.has_value());
    CHECK(noModes->displayModes.empty());
    CHECK_FALSE(preferredDisplayMode(noModes->displayModes).has_value());
}

TEST_CASE("display-mode rows without a usable size are skipped", "[moonlight][xml]") {
    const auto info = parseServerInfo("<root status_code=\"200\"><hostname>h</hostname>"
                                      "<SupportedDisplayMode>"
                                      "<DisplayMode><Width>0</Width><Height>0</Height>"
                                      "<RefreshRate>60</RefreshRate></DisplayMode>"
                                      "<DisplayMode><Height>768</Height></DisplayMode>"
                                      "<DisplayMode><Width>1024</Width><Height>768</Height>"
                                      "</DisplayMode>"
                                      "</SupportedDisplayMode></root>");
    REQUIRE(info.has_value());
    REQUIRE(info->displayModes.size() == 1);
    CHECK(info->displayModes[0].width == 1024);
    CHECK(info->displayModes[0].refreshRate == 0);
    const auto best = preferredDisplayMode(info->displayModes);
    REQUIRE(best.has_value());
    CHECK(best->height == 768);
}

TEST_CASE("an endpoint that names no status_code is still read", "[moonlight][xml]") {
    // Wolf's /applist answers plainly, with no status_code attribute at all.
    const auto apps = parseAppList("<root>"
                                   "<App><AppTitle>Desktop</AppTitle><ID>881448767</ID></App>"
                                   "</root>");
    REQUIRE(apps.size() == 1);
    CHECK(apps[0].id == "881448767");
    const auto info = parseServerInfo("<root><hostname>wolf</hostname>"
                                      "<PairStatus>1</PairStatus></root>");
    REQUIRE(info.has_value());
    CHECK(info->pairStatus == 1);
    const auto launch = parseLaunch("<root><sessionUrl0>rtsp://10.0.0.5:48010</sessionUrl0>"
                                    "<gamesession>1</gamesession></root>");
    REQUIRE(launch.has_value());
    CHECK(launch->rtspPort == 48010);
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
