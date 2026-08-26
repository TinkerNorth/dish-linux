// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Request strings are pinned exactly — the host side parses them with a strict
// grammar — and responses are parsed in both the \r\n and bare \n forms hosts
// emit.

#include "core/moonlight/MoonlightRtsp.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
    CHECK(req.find("\r\n\r\nv=0\r\n") != std::string::npos);
}

TEST_CASE("ANNOUNCE payload carries the WHOLE attribute set", "[moonlight][rtsp]") {
    // IT HAS TO BE THE WHOLE SET. A host builds its stream configuration by
    // looking each attribute up by name and a lookup that misses is fatal:
    // measured against a live Sunshine host, an ANNOUNCE carrying only the
    // seven attributes this client itself cares about is answered
    // 400 BAD REQUEST, while this set is answered 200 OK. Nothing here is
    // decoration, and an attribute dropped as unused is a host that stops
    // talking to us.
    StreamConfig config;
    config.width = 2560;
    config.height = 1440;
    config.fps = 120;
    const std::string p = buildAnnouncePayload(config);

    const std::vector<std::string> expected = {
        "v=0",
        "o=android 0 14 IN IPv4 0.0.0.0",
        "s=NVIDIA Streaming Client",
        "a=x-nv-video[0].clientViewportWd:2560",
        "a=x-nv-video[0].clientViewportHt:1440",
        "a=x-nv-video[0].maxFPS:120",
        "a=x-nv-video[0].packetSize:1024",
        "a=x-nv-video[0].rateControlMode:4",
        "a=x-nv-video[0].timeoutLengthMs:7000",
        "a=x-nv-video[0].framesWithInvalidRefThreshold:0",
        "a=x-nv-video[0].refPicInvalidation:0",
        "a=x-nv-video[0].encoderCscMode:0",
        "a=x-nv-video[0].dynamicRangeMode:0",
        "a=x-nv-video[0].maxNumReferenceFrames:1",
        "a=x-nv-video[0].videoEncoderSlicesPerFrame:1",
        "a=x-nv-video[0].clientRefreshRateX100:12000",
        "a=x-nv-vqos[0].bitStreamFormat:0",
        "a=x-nv-vqos[0].bw.minimumBitrateKbps:500",
        "a=x-nv-vqos[0].bw.maximumBitrateKbps:500",
        "a=x-nv-vqos[0].fec.enable:1",
        "a=x-nv-vqos[0].fec.minRequiredFecPackets:2",
        "a=x-nv-vqos[0].fec.repairPercent:20",
        "a=x-nv-vqos[0].drc.enable:0",
        "a=x-nv-vqos[0].videoQualityScoreUpdateTime:5000",
        "a=x-nv-vqos[0].qosTrafficType:5",
        "a=x-nv-aqos.qosTrafficType:4",
        "a=x-nv-aqos.packetDuration:5",
        "a=x-nv-audio.surround.numChannels:2",
        "a=x-nv-audio.surround.channelMask:3",
        "a=x-nv-audio.surround.enable:0",
        "a=x-nv-audio.surround.AudioQuality:0",
        "a=x-nv-general.useReliableUdp:13",
        "a=x-nv-general.featureFlags:167",
        "a=x-ml-general.featureFlags:3",
        "a=x-ss-general.encryptionEnabled:0",
        "t=0 0",
    };

    // Every line, in order, CRLF-terminated, and nothing else.
    std::string rebuilt;
    for (const auto& line : expected) {
        CHECK(p.find(line + "\r\n") != std::string::npos);
        rebuilt += line + "\r\n";
    }
    CHECK(p == rebuilt);

    // Thirty-two attributes, plus the v/o/s/t framing lines.
    std::size_t attributes = 0;
    for (std::size_t at = 0; (at = p.find("\na=", at)) != std::string::npos; ++at) { ++attributes; }
    CHECK(attributes == 32);
    CHECK(expected.size() == 36);
    CHECK(p.find("v=0\r\n") == 0);
}

TEST_CASE("ANNOUNCE payload follows the negotiated display mode", "[moonlight][rtsp]") {
    StreamConfig config;
    CHECK(config.width == 1920);
    CHECK(config.height == 1080);
    CHECK(config.fps == 60);
    const std::string p = buildAnnouncePayload(config);
    CHECK(p.find("a=x-nv-video[0].clientViewportWd:1920\r\n") != std::string::npos);
    CHECK(p.find("a=x-nv-video[0].clientViewportHt:1080\r\n") != std::string::npos);
    CHECK(p.find("a=x-nv-video[0].maxFPS:60\r\n") != std::string::npos);
    CHECK(p.find("a=x-nv-video[0].clientRefreshRateX100:6000\r\n") != std::string::npos);
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
    SECTION("connect data that is not a number") {
        const auto resp =
            parseResponse("RTSP/1.0 200 OK\r\nCSeq: 3\r\nX-SS-Connect-Data: 12ab34\r\n\r\n");
        REQUIRE(resp.has_value());
        CHECK_FALSE(connectData(*resp).has_value());
    }
    SECTION("connect data wider than 64 bits") {
        const auto resp = parseResponse("RTSP/1.0 200 OK\r\nCSeq: 3\r\n"
                                        "X-SS-Connect-Data: 999999999999999999999999\r\n\r\n");
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

TEST_CASE("X-SS-Connect-Data parses unsigned, above and below INT32_MAX", "[moonlight][rtsp]") {
    // READ WIDE, THEN NARROW. The token is unsigned 32-bit and a real host's
    // routinely sits above INT32_MAX: 4270471497 came off a live Sunshine host.
    // A signed parse fails for exactly those values and, defaulted, hands the
    // control stream a token of 0 — which connects, to the wrong session.
    const auto token = [](const std::string& text) {
        const auto resp =
            parseResponse("RTSP/1.0 200 OK\r\nCSeq: 3\r\nX-SS-Connect-Data: " + text + "\r\n\r\n");
        REQUIRE(resp.has_value());
        return connectData(*resp);
    };

    // Below INT32_MAX.
    CHECK(token("0") == 0U);
    CHECK(token("1") == 1U);
    CHECK(token("3735928559") == 3735928559U);
    CHECK(token("2147483646") == 2147483646U);
    CHECK(token("2147483647") == 2147483647U); // INT32_MAX itself

    // Above INT32_MAX — the half a signed parse loses.
    CHECK(token("2147483648") == 2147483648U);
    CHECK(token("4270471497") == 4270471497U); // the live host's own value
    CHECK(token("4294967295") == 4294967295U); // UINT32_MAX

    // Wider than the wire field: narrowed to the 32 bits ENet carries, never
    // silently dropped to zero.
    CHECK(token("4294967296") == 0U);
    CHECK(token("4294967297") == 1U);
    CHECK(token("99999999999") == static_cast<std::uint32_t>(99999999999ULL & 0xFFFFFFFFULL));

    // Whitespace around the value is tolerated the way hosts emit it.
    CHECK(token(" 4270471497 ") == 4270471497U);
}

TEST_CASE("Content-length frames a reply that declares one", "[moonlight][rtsp]") {
    const auto declared = parseResponse("RTSP/1.0 200 OK\r\nCSeq: 4\r\n"
                                        "Content-length: 11\r\n\r\nhello world");
    REQUIRE(declared.has_value());
    CHECK(contentLength(*declared) == 11);
    CHECK(declared->payload == "hello world");

    // The DESCRIBE shape: no Content-length at all, framed by the close.
    const auto framedByClose = parseResponse("RTSP/1.0 200 OK\r\nCSeq: 2\r\n\r\n"
                                             "a=fmtp:97 surround-params=21101\r\n");
    REQUIRE(framedByClose.has_value());
    CHECK_FALSE(contentLength(*framedByClose).has_value());
    CHECK(framedByClose->payload.find("surround-params") != std::string::npos);

    // Case-insensitive, and a non-numeric length is no length.
    const auto lowercase =
        parseResponse("RTSP/1.0 200 OK\r\nCSeq: 4\r\ncontent-length: 3\r\n\r\nabc");
    REQUIRE(lowercase.has_value());
    CHECK(contentLength(*lowercase) == 3);
    const auto rubbish =
        parseResponse("RTSP/1.0 200 OK\r\nCSeq: 4\r\nContent-length: many\r\n\r\nabc");
    REQUIRE(rubbish.has_value());
    CHECK_FALSE(contentLength(*rubbish).has_value());
}

TEST_CASE("option lookup is case-insensitive", "[moonlight][rtsp]") {
    const auto resp =
        parseResponse("RTSP/1.0 200 OK\r\nCSeq: 1\r\nsession: ABC;timeout=90\r\n\r\n");
    REQUIRE(resp.has_value());
    CHECK(sessionId(*resp) == "ABC");
}
