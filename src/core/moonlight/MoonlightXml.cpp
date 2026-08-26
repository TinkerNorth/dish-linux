// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/moonlight/MoonlightXml.h"

#include <cctype>
#include <cstdlib>

namespace dish::moonxml {
namespace {

std::string decodeEntities(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '&') {
            out.push_back(raw[i]);
            ++i;
            continue;
        }
        const auto tryEntity = [&](std::string_view entity, char replacement) {
            if (raw.compare(i, entity.size(), entity) == 0) {
                out.push_back(replacement);
                i += entity.size();
                return true;
            }
            return false;
        };
        if (tryEntity("&amp;", '&') || tryEntity("&lt;", '<') || tryEntity("&gt;", '>') ||
            tryEntity("&quot;", '"') || tryEntity("&apos;", '\'')) {
            continue;
        }
        out.push_back(raw[i]);
        ++i;
    }
    return out;
}

std::optional<int> parseInt(std::string_view text) {
    if (text.empty()) { return std::nullopt; }
    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        i = 1;
        if (text.size() == 1) { return std::nullopt; }
    }
    long value = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') { return std::nullopt; }
        value = value * 10 + (c - '0');
        if (value > 2147483647L) { return std::nullopt; }
    }
    return static_cast<int>(negative ? -value : value);
}

// The raw inner text of the first <tag ...>...</tag>, or nullopt.
std::optional<std::string_view> rawTagValue(std::string_view xml, std::string_view tag) {
    const std::string open = "<" + std::string(tag);
    const std::string close = "</" + std::string(tag) + ">";
    std::size_t at = 0;
    while ((at = xml.find(open, at)) != std::string_view::npos) {
        const std::size_t afterName = at + open.size();
        // Reject partial matches like <AppTitle> when looking for <App>.
        if (afterName < xml.size() && xml[afterName] != '>' && xml[afterName] != ' ' &&
            xml[afterName] != '/') {
            at = afterName;
            continue;
        }
        const std::size_t gt = xml.find('>', afterName);
        if (gt == std::string_view::npos) { return std::nullopt; }
        const std::size_t end = xml.find(close, gt + 1);
        if (end == std::string_view::npos) { return std::nullopt; }
        return xml.substr(gt + 1, end - gt - 1);
    }
    return std::nullopt;
}

// A reply that names no status_code is a plain success.
constexpr int kDefaultOk = 200;

bool containsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) { return false; }
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j])) { ++j; }
        if (j == needle.size()) { return true; }
    }
    return false;
}

// Every <DisplayMode> block inside <SupportedDisplayMode>, in document order.
std::vector<DisplayMode> parseDisplayModes(std::string_view xml) {
    std::vector<DisplayMode> modes;
    const auto listed = rawTagValue(xml, "SupportedDisplayMode");
    if (!listed) { return modes; }
    std::string_view rest = *listed;
    while (true) {
        const std::size_t open = rest.find("<DisplayMode>");
        if (open == std::string_view::npos) { break; }
        const std::size_t close = rest.find("</DisplayMode>", open);
        if (close == std::string_view::npos) { break; }
        const std::string_view block = rest.substr(open, close - open);
        DisplayMode mode;
        mode.width = tagInt(block, "Width").value_or(0);
        mode.height = tagInt(block, "Height").value_or(0);
        mode.refreshRate = tagInt(block, "RefreshRate").value_or(0);
        if (mode.width > 0 && mode.height > 0) { modes.push_back(mode); }
        rest = rest.substr(close + 1);
    }
    return modes;
}

} // namespace

std::optional<std::string> tagValue(std::string_view xml, std::string_view tag) {
    const auto raw = rawTagValue(xml, tag);
    if (!raw) { return std::nullopt; }
    return decodeEntities(*raw);
}

std::optional<int> tagInt(std::string_view xml, std::string_view tag) {
    const auto raw = rawTagValue(xml, tag);
    if (!raw) { return std::nullopt; }
    return parseInt(*raw);
}

std::optional<int> statusCode(std::string_view xml) {
    static constexpr std::string_view kAttr = "status_code=\"";
    const std::size_t at = xml.find(kAttr);
    if (at == std::string_view::npos) { return std::nullopt; }
    const std::size_t start = at + kAttr.size();
    const std::size_t end = xml.find('"', start);
    if (end == std::string_view::npos) { return std::nullopt; }
    return parseInt(xml.substr(start, end - start));
}

std::optional<std::string> statusMessage(std::string_view xml) {
    static constexpr std::string_view kAttr = "status_message=\"";
    const std::size_t at = xml.find(kAttr);
    if (at == std::string_view::npos) { return std::nullopt; }
    const std::size_t start = at + kAttr.size();
    const std::size_t end = xml.find('"', start);
    if (end == std::string_view::npos) { return std::nullopt; }
    return decodeEntities(xml.substr(start, end - start));
}

bool Status::appAlreadyRunning() const {
    return !ok() && containsNoCase(message, "already running");
}

std::optional<Status> parseStatus(std::string_view xml) {
    if (xml.find("<root") == std::string_view::npos) { return std::nullopt; }
    Status status;
    status.code = statusCode(xml).value_or(kDefaultOk);
    status.message = statusMessage(xml).value_or("");
    status.resume = tagInt(xml, "resume").value_or(0) == 1;
    return status;
}

std::optional<DisplayMode> preferredDisplayMode(const std::vector<DisplayMode>& modes) {
    std::optional<DisplayMode> best;
    for (const auto& mode : modes) {
        if (mode.width <= 0 || mode.height <= 0) { continue; }
        if (!best) {
            best = mode;
            continue;
        }
        const long area = static_cast<long>(mode.width) * mode.height;
        const long bestArea = static_cast<long>(best->width) * best->height;
        if (area > bestArea || (area == bestArea && mode.refreshRate > best->refreshRate)) {
            best = mode;
        }
    }
    return best;
}

std::optional<ServerInfo> parseServerInfo(std::string_view xml) {
    if (statusCode(xml).value_or(kDefaultOk) != 200) { return std::nullopt; }
    const auto hostname = tagValue(xml, "hostname");
    if (!hostname || hostname->empty()) { return std::nullopt; }
    ServerInfo info;
    info.hostname = *hostname;
    info.uuid = tagValue(xml, "uniqueid").value_or("");
    info.appVersion = tagValue(xml, "appversion").value_or("");
    info.state = tagValue(xml, "state").value_or("");
    info.httpsPort = tagInt(xml, "HttpsPort").value_or(0);
    info.externalPort = tagInt(xml, "ExternalPort").value_or(0);
    info.pairStatus = tagInt(xml, "PairStatus").value_or(0);
    info.currentGame = tagInt(xml, "currentgame").value_or(0);
    info.displayModes = parseDisplayModes(xml);
    return info;
}

std::vector<AppEntry> parseAppList(std::string_view xml) {
    std::vector<AppEntry> apps;
    if (statusCode(xml).value_or(kDefaultOk) != 200) { return apps; }
    std::size_t at = 0;
    while (true) {
        const std::size_t open = xml.find("<App>", at);
        if (open == std::string_view::npos) { break; }
        const std::size_t close = xml.find("</App>", open);
        if (close == std::string_view::npos) { break; }
        const std::string_view block = xml.substr(open, close - open + 6);
        const auto title = tagValue(block, "AppTitle");
        const auto id = tagValue(block, "ID");
        if (title && id && !id->empty()) { apps.push_back(AppEntry{*title, *id}); }
        at = close + 6;
    }
    return apps;
}

std::optional<LaunchResult> parseLaunch(std::string_view xml) {
    const auto status = parseStatus(xml);
    if (!status || !status->ok()) { return std::nullopt; }
    const auto url = tagValue(xml, "sessionUrl0");
    if (!url || url->empty()) { return std::nullopt; }

    // scheme://host[:port] — the scheme varies by host implementation, so only
    // the host and port are read.
    std::string_view rest = *url;
    const std::size_t schemeEnd = rest.find("://");
    if (schemeEnd != std::string_view::npos) { rest = rest.substr(schemeEnd + 3); }
    LaunchResult result;
    const std::size_t colon = rest.rfind(':');
    if (colon != std::string_view::npos) {
        const auto port = parseInt(rest.substr(colon + 1));
        if (!port || *port <= 0 || *port > 65535) { return std::nullopt; }
        result.rtspPort = *port;
        result.rtspHost = std::string(rest.substr(0, colon));
    } else {
        result.rtspHost = std::string(rest);
    }
    if (result.rtspHost.empty()) { return std::nullopt; }
    result.launched =
        tagInt(xml, "gamesession").value_or(0) == 1 || tagInt(xml, "resume").value_or(0) == 1;
    return result;
}

bool pairedFlag(std::string_view xml) { return tagInt(xml, "paired").value_or(0) == 1; }

} // namespace dish::moonxml
