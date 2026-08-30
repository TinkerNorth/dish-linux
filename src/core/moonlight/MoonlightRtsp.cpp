// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/moonlight/MoonlightRtsp.h"

#include <algorithm>
#include <cctype>

namespace dish::moonrtsp {
namespace {

// The client version real Moonlight 5.x clients advertise.
constexpr const char* kClientVersion = "14";

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<int> parseInt(std::string_view text) {
    text = trimmed(text);
    if (text.empty()) { return std::nullopt; }
    long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') { return std::nullopt; }
        value = value * 10 + (c - '0');
        if (value > 2147483647L) { return std::nullopt; }
    }
    return static_cast<int>(value);
}

std::string requestHead(const std::string& cmd, const std::string& target, int cseq,
                        const std::string& sessionId) {
    std::string out = cmd + " " + target + " RTSP/1.0\r\n";
    out += "CSeq: " + std::to_string(cseq) + "\r\n";
    out += "X-GS-ClientVersion: ";
    out += kClientVersion;
    out += "\r\n";
    if (!sessionId.empty()) { out += "Session: " + sessionId + "\r\n"; }
    return out;
}

} // namespace

std::string formatOptions(int cseq, const std::string& target) {
    return requestHead("OPTIONS", target, cseq, "") + "\r\n";
}

std::string formatDescribe(int cseq, const std::string& target) {
    std::string out = requestHead("DESCRIBE", target, cseq, "");
    out += "Accept: application/sdp\r\n";
    out += "\r\n";
    return out;
}

std::string formatSetup(int cseq, const std::string& streamId, const std::string& sessionId) {
    std::string out = requestHead("SETUP", "streamid=" + streamId + "/0/0", cseq, sessionId);
    // The host assigns the real ports; the client-port hint rides along for
    // parity with real clients.
    out += "Transport: unicast;X-GS-ClientPort=50000-50001\r\n";
    out += "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n";
    out += "\r\n";
    return out;
}

std::string formatAnnounce(int cseq, const std::string& sessionId, const std::string& payload) {
    std::string out = requestHead("ANNOUNCE", "streamid=control/13/0", cseq, sessionId);
    out += "Content-type: application/sdp\r\n";
    out += "Content-length: " + std::to_string(payload.size()) + "\r\n";
    out += "\r\n";
    out += payload;
    return out;
}

std::string formatPlay(int cseq, const std::string& target, const std::string& sessionId) {
    return requestHead("PLAY", target, cseq, sessionId) + "\r\n";
}

std::string buildAnnouncePayload(const StreamConfig& config) {
    // The WHOLE attribute set a real client sends. A host builds its stream
    // configuration by looking each of these up by name and a lookup that
    // misses is fatal: measured against a live Sunshine host, an ANNOUNCE
    // carrying only the handful of attributes this client cares about is
    // answered 400 BAD REQUEST while this set is answered 200 OK, with either
    // line ending. Nothing here is decoration.
    std::string p;
    const auto line = [&p](const std::string& text) { p += text + "\r\n"; };
    const auto arg = [&line](const std::string& key, int value) {
        line("a=" + key + ":" + std::to_string(value));
    };
    line("v=0");
    line("o=android 0 14 IN IPv4 0.0.0.0");
    line("s=NVIDIA Streaming Client");
    arg("x-nv-video[0].clientViewportWd", config.width);
    arg("x-nv-video[0].clientViewportHt", config.height);
    arg("x-nv-video[0].maxFPS", config.fps);
    arg("x-nv-video[0].packetSize", config.packetSize);
    arg("x-nv-video[0].rateControlMode", 4);
    arg("x-nv-video[0].timeoutLengthMs", 7000);
    arg("x-nv-video[0].framesWithInvalidRefThreshold", 0);
    arg("x-nv-video[0].refPicInvalidation", 0);
    arg("x-nv-video[0].encoderCscMode", 0);
    arg("x-nv-video[0].dynamicRangeMode", 0);
    arg("x-nv-video[0].maxNumReferenceFrames", 1);
    arg("x-nv-video[0].videoEncoderSlicesPerFrame", 1);
    arg("x-nv-video[0].clientRefreshRateX100", config.fps * 100);
    arg("x-nv-vqos[0].bitStreamFormat", 0); // H.264, every host's floor
    arg("x-nv-vqos[0].bw.minimumBitrateKbps", config.bitrateKbps);
    arg("x-nv-vqos[0].bw.maximumBitrateKbps", config.bitrateKbps);
    arg("x-nv-vqos[0].fec.enable", 1);
    arg("x-nv-vqos[0].fec.minRequiredFecPackets", 2);
    arg("x-nv-vqos[0].fec.repairPercent", 20);
    arg("x-nv-vqos[0].drc.enable", 0);
    arg("x-nv-vqos[0].videoQualityScoreUpdateTime", 5000);
    arg("x-nv-vqos[0].qosTrafficType", 5);
    arg("x-nv-aqos.qosTrafficType", 4);
    arg("x-nv-aqos.packetDuration", 5);
    arg("x-nv-audio.surround.numChannels", config.audioChannels);
    arg("x-nv-audio.surround.channelMask", 3);
    arg("x-nv-audio.surround.enable", 0);
    arg("x-nv-audio.surround.AudioQuality", 0);
    arg("x-nv-general.useReliableUdp", 13);
    arg("x-nv-general.featureFlags", 167);
    arg("x-ml-general.featureFlags", 3);
    arg("x-ss-general.encryptionEnabled", 0);
    line("t=0 0");
    return p;
}

std::optional<std::string> Response::option(std::string_view name) const {
    for (const auto& [key, value] : options) {
        if (iequals(key, name)) { return value; }
    }
    return std::nullopt;
}

std::optional<Response> parseResponse(std::string_view text) {
    static constexpr std::string_view kProto = "RTSP/";
    if (text.substr(0, kProto.size()) != kProto) { return std::nullopt; }

    Response resp;
    std::size_t lineEnd = text.find('\n');
    std::string_view statusLine =
        trimmed(text.substr(0, lineEnd == std::string_view::npos ? text.size() : lineEnd));
    // "RTSP/1.0 200 OK"
    const std::size_t firstSpace = statusLine.find(' ');
    if (firstSpace == std::string_view::npos) { return std::nullopt; }
    std::string_view afterProto = statusLine.substr(firstSpace + 1);
    const std::size_t secondSpace = afterProto.find(' ');
    const std::string_view codeText =
        secondSpace == std::string_view::npos ? afterProto : afterProto.substr(0, secondSpace);
    const auto code = parseInt(codeText);
    if (!code) { return std::nullopt; }
    resp.statusCode = *code;
    if (secondSpace != std::string_view::npos) {
        resp.statusMessage = std::string(trimmed(afterProto.substr(secondSpace + 1)));
    }
    if (lineEnd == std::string_view::npos) { return resp; }

    std::size_t pos = lineEnd + 1;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) { end = text.size(); }
        const std::string_view line = trimmed(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty()) { break; } // end of options; the rest is payload
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) { continue; }
        const std::string_view key = trimmed(line.substr(0, colon));
        const std::string_view value = trimmed(line.substr(colon + 1));
        if (iequals(key, "CSeq")) {
            resp.cseq = parseInt(value).value_or(0);
        } else {
            resp.options.emplace_back(std::string(key), std::string(value));
        }
    }
    if (pos < text.size()) { resp.payload = std::string(text.substr(pos)); }
    return resp;
}

std::optional<int> transportPort(const Response& response) {
    const auto transport = response.option("Transport");
    if (!transport) { return std::nullopt; }
    static constexpr std::string_view kKey = "server_port=";
    const std::size_t at = transport->find(kKey);
    if (at == std::string::npos) { return std::nullopt; }
    std::string_view rest = std::string_view(*transport).substr(at + kKey.size());
    const std::size_t end = rest.find_first_not_of("0123456789");
    if (end != std::string_view::npos) { rest = rest.substr(0, end); }
    const auto port = parseInt(rest);
    if (!port || *port <= 0 || *port > 65535) { return std::nullopt; }
    return port;
}

std::optional<std::uint32_t> connectData(const Response& response) {
    const auto value = response.option("X-SS-Connect-Data");
    if (!value) { return std::nullopt; }
    const std::string_view text = trimmed(*value);
    if (text.empty()) { return std::nullopt; }
    // Read wide, then narrow. The token is unsigned 32-bit and a real host's
    // routinely sits above INT32_MAX (4270471497 came off a live Sunshine
    // host), so a signed parse yields nothing and the control stream connects
    // with a token of 0.
    unsigned long long parsed = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') { return std::nullopt; }
        if (parsed > (0xFFFFFFFFFFFFFFFFULL - static_cast<unsigned long long>(c - '0')) / 10ULL) {
            return std::nullopt;
        }
        parsed = parsed * 10ULL + static_cast<unsigned long long>(c - '0');
    }
    return static_cast<std::uint32_t>(parsed & 0xFFFFFFFFULL);
}

std::optional<int> contentLength(const Response& response) {
    const auto value = response.option("Content-length");
    if (!value) { return std::nullopt; }
    const auto parsed = parseInt(*value);
    if (!parsed || *parsed < 0) { return std::nullopt; }
    return parsed;
}

std::optional<std::string> pingPayload(const Response& response) {
    return response.option("X-SS-Ping-Payload");
}

std::optional<std::string> sessionId(const Response& response) {
    const auto value = response.option("Session");
    if (!value) { return std::nullopt; }
    const std::size_t semi = value->find(';');
    std::string id = semi == std::string::npos ? *value : value->substr(0, semi);
    const std::string_view t = trimmed(id);
    if (t.empty()) { return std::nullopt; }
    return std::string(t);
}

} // namespace dish::moonrtsp
