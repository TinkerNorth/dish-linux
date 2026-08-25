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

    bool busy() const { return state.find("_SERVER_BUSY") != std::string::npos; }
};

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
