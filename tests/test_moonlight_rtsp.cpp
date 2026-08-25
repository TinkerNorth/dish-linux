// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Request strings are pinned exactly — the host side parses them with a strict
// grammar — and responses are parsed in both the \r\n and bare \n forms hosts
// emit.

#include "core/moonlight/MoonlightRtsp.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace dish::moonrtsp;

TEST_CASE("OPTIONS request formatting", "[moonlight][rtsp]") {
    const std::string req = formatOptions(1, "rtsp://192.168.1.100:48010");
    CHECK(req == "OPTIONS rtsp://192.168.1.100:48010 RTSP/1.0\r\n"
                 "CSeq: 1\r\n"
                 "X-GS-ClientVersion: 14\r\n"
                 "\r\n");
}

TEST_CASE("DESCRIBE request formatting", "[moonlight][rtsp]") {
    const std::string req = formatDescribe(2, "rtsp://10.0.0.7:48010");
    CHECK(req == "DESCRIBE rtsp://10.0.0.7:48010 RTSP/1.0\r\n"
                 "CSeq: 2\r\n"
                 "X-GS-ClientVersion: 14\r\n"
                 "Accept: application/sdp\r\n"
                 "\r\n");
}

TEST_CASE("SETUP request formatting, with and without a session", "[moonlight][rtsp]") {
    CHECK(formatSetup(3, "audio", "") == "SETUP streamid=audio/0/0 RTSP/1.0\r\n"
                                         "CSeq: 3\r\n"
                                         "X-GS-ClientVersion: 14\r\n"
                                         "Transport: unicast;X-GS-ClientPort=50000-50001\r\n"
                                         "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                                         "\r\n");
    const std::string withSession = formatSetup(4, "control", "DEADBEEFCAFE");
    CHECK(withSession.find("SETUP streamid=control/0/0 RTSP/1.0\r\n") == 0);
    CHECK(withSession.find("Session: DEADBEEFCAFE\r\n") != std::string::npos);
}

TEST_CASE("ANNOUNCE request carries the SDP payload", "[moonlight][rtsp]") {
    const std::string payload = buildAnnouncePayload(StreamConfig{});
    const std::string req = formatAnnounce(6, "DEADBEEFCAFE", payload);
    CHECK(req.find("ANNOUNCE streamid=control/13/0 RTSP/1.0\r\n") == 0);
    CHECK(req.find("Content-type: application/sdp\r\n") != std::string::npos);
    CHECK(req.find("Content-length: " + std::to_string(payload.size()) + "\r\n") !=
          std::string::npos);
    // The payload rides after the blank line.
    CHECK(req.find("\r\n\r\nv=0\n") != std::string::npos);
}

TEST_CASE("ANNOUNCE payload advertises the floor stream settings", "[moonlight][rtsp]") {
    StreamConfig config;
    const std::string p = buildAnnouncePayload(config);
    // The three arguments host session setup requires…
    CHECK(p.find("a=x-nv-video[0].clientViewportWd:1280 \n") != std::string::npos);
    CHECK(p.find("a=x-nv-video[0].clientViewportHt:720 \n") != std::string::npos);
    CHECK(p.find("a=x-nv-video[0].maxFPS:30 \n") != std::string::npos);
    // …plus the floor codec/audio choices.
    CHECK(p.find("a=x-nv-vqos[0].bitStreamFormat:0 \n") != std::string::npos);
    CHECK(p.find("a=x-nv-audio.surround.numChannels:2 \n") != std::string::npos);
    CHECK(p.find("a=x-nv-vqos[0].bw.maximumBitrateKbps:1000 \n") != std::string::npos);
    // SDP framing lines the grammar expects.
    CHECK(p.find("v=0\n") == 0);
    CHECK(p.find("t=0 0\n") != std::string::npos);
}

TEST_CASE("PLAY request formatting", "[moonlight][rtsp]") {
    const std::string req = formatPlay(7, "rtsp://192.168.1.100:48010", "DEADBEEFCAFE");
    CHECK(req == "PLAY rtsp://192.168.1.100:48010 RTSP/1.0\r\n"
                 "CSeq: 7\r\n"
                 "X-GS-ClientVersion: 14\r\n"
                 "Session: DEADBEEFCAFE\r\n"
                 "\r\n");
}

TEST_CASE("parses a CRLF response with options", "[moonlight][rtsp]") {
    const auto resp = parseResponse("RTSP/1.0 200 OK\r\n"
                                    "CSeq: 5\r\n"
                                    "Session: DEADBEEFCAFE;timeout = 90\r\n"
                                    "Transport: server_port=47999\r\n"
                                    "X-SS-Connect-Data: 3735928559\r\n"
                                    "\r\n");
    REQUIRE(resp.has_value());
    CHECK(resp->ok());
    CHECK(resp->statusCode == 200);
    CHECK(resp->statusMessage == "OK");
    CHECK(resp->cseq == 5);
    CHECK(transportPort(*resp) == 47999);
    CHECK(connectData(*resp) == 3735928559U);
    CHECK(sessionId(*resp) == "DEADBEEFCAFE");
}

TEST_CASE("parses a bare-LF response with payload", "[moonlight][rtsp]") {
    const auto resp = parseResponse("RTSP/1.0 200 OK\n"
                                    "CSeq: 2\n"
                                    "\n"
                                    "sprop-parameter-sets=AAAAAU\n"
                                    "a=fmtp:97 surround-params=21101\n");
    REQUIRE(resp.has_value());
    CHECK(resp->ok());
    CHECK(resp->cseq == 2);
    CHECK(resp->payload.find("surround-params=21101") != std::string::npos);
}

TEST_CASE("parses an error response", "[moonlight][rtsp]") {
    const auto resp = parseResponse("RTSP/1.0 404 NOT FOUND\r\nCSeq: 9\r\n\r\n");
    REQUIRE(resp.has_value());
    CHECK_FALSE(resp->ok());
    CHECK(resp->statusCode == 404);
    CHECK(resp->statusMessage == "NOT FOUND");
    CHECK(resp->cseq == 9);
}

TEST_CASE("rejects non-response input", "[moonlight][rtsp]") {
    CHECK_FALSE(parseResponse("").has_value());
    CHECK_FALSE(
        parseResponse("OPTIONS rtsp://1.2.3.4:48010 RTSP/1.0\r\nCSeq: 1\r\n\r\n").has_value());
    CHECK_FALSE(parseResponse("RTSP/1.0").has_value());
    CHECK_FALSE(parseResponse("RTSP/1.0 abc OK").has_value());
}

TEST_CASE("transport parsing is robust", "[moonlight][rtsp]") {
    SECTION("port with trailing parameters") {
        const auto resp = parseResponse("RTSP/1.0 200 OK\r\nCSeq: 3\r\n"
                                        "Transport: server_port=48000;mode=play\r\n\r\n");
        REQUIRE(resp.has_value());
        CHECK(transportPort(*resp) == 48000);
    }
    SECTION("missing Transport option") {
        const auto resp = parseResponse("RTSP/1.0 200 OK\r\nCSeq: 3\r\n\r\n");
        REQUIRE(resp.has_value());
        CHECK_FALSE(transportPort(*resp).has_value());
        CHECK_FALSE(connectData(*resp).has_value());
        CHECK_FALSE(sessionId(*resp).has_value());
        CHECK_FALSE(pingPayload(*resp).has_value());
    }
    SECTION("out-of-range port") {
        const auto resp =
            parseResponse("RTSP/1.0 200 OK\r\nCSeq: 3\r\nTransport: server_port=70000\r\n\r\n");
        REQUIRE(resp.has_value());
        CHECK_FALSE(transportPort(*resp).has_value());
    }
    SECTION("connect data overflow") {
        const auto resp =
            parseResponse("RTSP/1.0 200 OK\r\nCSeq: 3\r\nX-SS-Connect-Data: 99999999999\r\n\r\n");
        REQUIRE(resp.has_value());
        CHECK_FALSE(connectData(*resp).has_value());
    }
    SECTION("ping payload passes through verbatim") {
        const auto resp = parseResponse(
            "RTSP/1.0 200 OK\r\nCSeq: 3\r\nX-SS-Ping-Payload: AbCd0123EfGh4567\r\n\r\n");
        REQUIRE(resp.has_value());
        CHECK(pingPayload(*resp) == "AbCd0123EfGh4567");
    }
}

TEST_CASE("option lookup is case-insensitive", "[moonlight][rtsp]") {
    const auto resp =
        parseResponse("RTSP/1.0 200 OK\r\nCSeq: 1\r\nsession: ABC;timeout=90\r\n\r\n");
    REQUIRE(resp.has_value());
    CHECK(sessionId(*resp) == "ABC");
}
