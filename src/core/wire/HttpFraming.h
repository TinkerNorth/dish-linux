// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One HTTP/1.1 exchange on a connection that closes after it: the request as bytes, and the
// response read back out of whatever the connection has delivered so far. Nothing else of HTTP
// lives here on purpose, no keep-alive, redirects, proxies or content codings, because the only
// peers are a satellite and a GameStream host on the LAN. The socket is source/http's; this is
// pure byte work.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dish::wire {

// A header field as {name, value}.
using HttpHeader = std::pair<std::string, std::string>;

// The request line, Host, Connection: close, `headers` in their order, and Content-Length and the
// body when there is one. `target` is the origin form ("/path?query") and `authority` the Host
// value ("host:port").
std::string formatHttpRequest(std::string_view method, std::string_view target,
                              std::string_view authority, const std::vector<HttpHeader>& headers,
                              std::string_view body);

struct HttpResponse {
    int status = 0;
    // Names are lower-cased as they are read, which is what makes every lookup case-insensitive.
    std::vector<HttpHeader> headers;
    std::string body;

    // The first field called `lowerName`, or empty.
    std::string header(std::string_view lowerName) const;
};

struct HttpParse {
    enum class State : std::uint8_t {
        Incomplete, // more bytes are needed, and the peer may still send them
        Complete,   // `response` is the whole final response
        Malformed,  // not HTTP/1.x, or the peer hung up before the response was whole
    };
    State state = State::Incomplete;
    HttpResponse response;
};

// Reads the final response out of `received`, all the connection has delivered so far; interim
// 1xx responses ahead of it are skipped. `peerClosed` says nothing more will arrive, which is what
// ends a body that declares no length, and what makes a response that is still short a broken one.
HttpParse parseHttpResponse(std::string_view received, bool peerClosed);

} // namespace dish::wire
