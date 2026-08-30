// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// RTSP request formatting and response parsing for the Moonlight handshake
// (OPTIONS -> DESCRIBE -> SETUP x3 -> ANNOUNCE -> PLAY over plaintext TCP).
// The request shapes mirror what real clients send and what Wolf's PEG parser
// (src/moonlight-protocol/rtsp/parser.hpp) accepts; responses are parsed
// leniently, accepting both \r\n and bare \n line endings since hosts emit
// both. The transport socket lives in source/moonlight; this is pure
// string work.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dish::moonrtsp {

// What the ANNOUNCE SDP advertises. The geometry is the smallest a host will
// take, and the same on all three Dish clients: nothing is decoded, so every
// pixel the host encodes is GPU time taken from the game the user is playing
// on that same machine. `sops=0` on the launch is what keeps the host from
// changing the user's display to match, so the size never reaches their
// desktop. The bitrate stays at the floor because video and audio are
// discarded.
struct StreamConfig {
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrateKbps = 500;
    int packetSize = 1024;
    int audioChannels = 2;
};

// `target` is the rtsp://host:port string the launch response handed out,
// parroted back verbatim — Wolf matches sessions on it, so it is never
// rewritten to the real address. `sessionId` is empty until the first SETUP
// response supplies one.
std::string formatOptions(int cseq, const std::string& target);
std::string formatDescribe(int cseq, const std::string& target);
// `streamId` is "audio", "video" or "control".
std::string formatSetup(int cseq, const std::string& streamId, const std::string& sessionId);
std::string formatAnnounce(int cseq, const std::string& sessionId, const std::string& payload);
std::string formatPlay(int cseq, const std::string& target, const std::string& sessionId);

std::string buildAnnouncePayload(const StreamConfig& config);

struct Response {
    int statusCode = 0;
    std::string statusMessage;
    int cseq = 0;
    std::vector<std::pair<std::string, std::string>> options;
    std::string payload;

    bool ok() const { return statusCode == 200; }
    // Case-insensitive header lookup.
    std::optional<std::string> option(std::string_view name) const;
};

// nullopt when `text` is not an RTSP response status line. Options and payload
// parse leniently: an option line without ':' is skipped, everything after the
// blank line is payload.
std::optional<Response> parseResponse(std::string_view text);

// SETUP: "Transport: server_port=NNNN[;...]" -> the port.
std::optional<int> transportPort(const Response& response);

// SETUP control: "X-SS-Connect-Data" -> the u32 the ENet connect carries.
// Parsed unsigned and 64 bits wide, then narrowed to the 32 bits on the wire.
std::optional<std::uint32_t> connectData(const Response& response);

// "Content-length" -> the declared payload size. Absent on the DESCRIBE reply,
// which the host frames by closing the connection instead.
std::optional<int> contentLength(const Response& response);

// SETUP audio/video: "X-SS-Ping-Payload" -> the 16-char payload the RTP ping
// datagrams echo. Absent on hosts that accept the legacy 4-byte PING.
std::optional<std::string> pingPayload(const Response& response);

// SETUP: "Session: DEADBEEFCAFE;timeout = 90" -> "DEADBEEFCAFE".
std::optional<std::string> sessionId(const Response& response);

} // namespace dish::moonrtsp
