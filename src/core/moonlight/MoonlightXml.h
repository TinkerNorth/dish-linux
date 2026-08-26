// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Parsers for the small, flat XML documents the GameStream HTTP API returns
// (serverinfo, pair phases, applist, launch/resume/cancel). The shapes come
// from Wolf's moonlight.cpp response builders; Sunshine and Apollo emit the
// same documents. A hand-rolled tag scanner keeps core/ Qt-free — the
// documents are machine-generated, non-nested apart from <App> blocks, and
// carry no namespaces, so a full XML parser buys nothing here.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dish::moonxml {

// First <tag>value</tag> occurrence, entities decoded. nullopt when absent.
std::optional<std::string> tagValue(std::string_view xml, std::string_view tag);

// tagValue parsed as a decimal integer; nullopt when absent or non-numeric.
std::optional<int> tagInt(std::string_view xml, std::string_view tag);

// The <root status_code="..."> attribute; nullopt when absent.
std::optional<int> statusCode(std::string_view xml);

// The <root status_message="..."> attribute; nullopt when absent.
std::optional<std::string> statusMessage(std::string_view xml);

// The application-level result every reply carries on its root element,
// independent of the HTTP status the transport reported. A host says no in the
// BODY: /launch answers HTTP 200 with status_code="400" and status_message="An
// app is already running on this host", so code that reads only the HTTP status
// treats a refusal as success and fails later, naming the wrong thing.
struct Status {
    int code = 200;
    std::string message;
    // The <resume> flag a refusal carries: 1 means /resume would be accepted.
    bool resume = false;

    bool ok() const { return code >= 200 && code <= 299; }
    // The host holds an app it will not start a second one beside.
    bool appAlreadyRunning() const;
};

// A reply that names no status_code is read as success, which is what a host
// that answers plainly sends. nullopt only when there is no root element.
std::optional<Status> parseStatus(std::string_view xml);

// One <SupportedDisplayMode><DisplayMode> row from /serverinfo.
struct DisplayMode {
    int width = 0;
    int height = 0;
    int refreshRate = 0;
};

// GET /serverinfo.
struct ServerInfo {
    std::string hostname;
    std::string uuid;
    std::string appVersion;
    std::string state; // e.g. SUNSHINE_SERVER_FREE / _BUSY
    int httpsPort = 0;
    int externalPort = 0;
    // 1 when THIS client (matched by uniqueid/cert) is already paired.
    int pairStatus = 0;
    // Running app id, 0 or -1 for none; drives launch-vs-resume.
    int currentGame = 0;
    // Every <SupportedDisplayMode> row, in document order.
    std::vector<DisplayMode> displayModes;

    bool busy() const { return state.find("_SERVER_BUSY") != std::string::npos; }
};

// The advertised mode closest to the host's own display: the largest area, and
// the highest refresh rate offered at that size. nullopt when none were
// advertised, so the caller keeps its own default rather than shrinking the
// host's desktop.
std::optional<DisplayMode> preferredDisplayMode(const std::vector<DisplayMode>& modes);

// nullopt when the document has no status_code 200 root or lacks a hostname.
std::optional<ServerInfo> parseServerInfo(std::string_view xml);

// GET /applist rows.
struct AppEntry {
    std::string title;
    std::string id;
};

std::vector<AppEntry> parseAppList(std::string_view xml);

// GET /launch and /resume.
struct LaunchResult {
    // The verbatim sessionUrl0 host — parroted back as the RTSP target (Wolf
    // hands out a per-session fake IP and matches on it), never dialled.
    std::string rtspHost;
    int rtspPort = 0;
    bool launched = false; // gamesession=1 or resume=1
};

std::optional<LaunchResult> parseLaunch(std::string_view xml);

// Pair phase responses share {paired, plaincert?, challengeresponse?,
// pairingsecret?}; callers pick the field for their phase via tagValue and
// check paired via this helper.
bool pairedFlag(std::string_view xml);

} // namespace dish::moonxml
