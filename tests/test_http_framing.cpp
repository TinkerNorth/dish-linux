// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The HTTP/1.1 bytes both REST clients write and read back, without a socket: every way an answer
// can frame its body, every point at which it can arrive short, and the shapes that are not an
// answer at all. test_http_client_wire.cpp drives the same framings through a real connection.

#include "core/wire/HttpFraming.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>

using dish::wire::formatHttpRequest;
using dish::wire::HttpParse;
using dish::wire::parseHttpResponse;

namespace {

using State = HttpParse::State;

// Cutting `whole` anywhere from `from` on leaves an answer that is still worth waiting for, and
// a broken one if the peer has already hung up.
void everyCutIsShort(std::string_view whole, std::size_t from) {
    for (std::size_t cut = from; cut < whole.size(); ++cut) {
        INFO("cut after " << cut << " bytes");
        CHECK(parseHttpResponse(whole.substr(0, cut), false).state == State::Incomplete);
        CHECK(parseHttpResponse(whole.substr(0, cut), true).state == State::Malformed);
    }
}

} // namespace

TEST_CASE("a request carries its line, Host, Connection: close, its headers and a sized body",
          "[http][framing]") {
    const std::string bytes = formatHttpRequest(
        "PUT", "/api/connections", "127.0.0.1:9443",
        {{"Content-Type", "application/json"}, {"X-Device-Id", "dev-1"}}, "{\"a\":1}");

    CHECK(bytes == "PUT /api/connections HTTP/1.1\r\n"
                   "Host: 127.0.0.1:9443\r\n"
                   "Connection: close\r\n"
                   "Content-Type: application/json\r\n"
                   "X-Device-Id: dev-1\r\n"
                   "Content-Length: 7\r\n"
                   "\r\n"
                   "{\"a\":1}");
}

TEST_CASE("a request with no body declares no length", "[http][framing]") {
    const std::string bytes =
        formatHttpRequest("GET", "/serverinfo?uniqueid=7b", "10.0.0.2:47989", {}, "");

    CHECK(bytes == "GET /serverinfo?uniqueid=7b HTTP/1.1\r\n"
                   "Host: 10.0.0.2:47989\r\n"
                   "Connection: close\r\n"
                   "\r\n");
}

TEST_CASE("a sized body is read to its length", "[http][framing]") {
    const HttpParse parse =
        parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":true}", false);

    REQUIRE(parse.state == State::Complete);
    CHECK(parse.response.status == 200);
    CHECK(parse.response.body == "{\"ok\":true}");
}

TEST_CASE("a sized answer is waited for until all of it is there", "[http][framing]") {
    everyCutIsShort("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":true}", 0);
}

TEST_CASE("a length that is not a whole number is not an answer", "[http][framing]") {
    const std::string_view lengths[] = {"12abc", "-1", "0x10", "99999999999999999999999"};
    for (const std::string_view length : lengths) {
        INFO("Content-Length: " << length);
        const std::string bytes =
            "HTTP/1.1 200 OK\r\nContent-Length: " + std::string(length) + "\r\n\r\nbody";
        CHECK(parseHttpResponse(bytes, false).state == State::Malformed);
    }
}

TEST_CASE("a chunked body is joined, its extensions and trailers passed over", "[http][framing]") {
    const HttpParse parse = parseHttpResponse("HTTP/1.1 200 OK\r\n"
                                              "Transfer-Encoding: chunked\r\n"
                                              "\r\n"
                                              "5;name=value\r\n"
                                              "{\"ok\"\r\n"
                                              "6\r\n"
                                              ":true}\r\n"
                                              "0\r\n"
                                              "Checksum: none\r\n"
                                              "\r\n",
                                              false);

    REQUIRE(parse.state == State::Complete);
    CHECK(parse.response.body == "{\"ok\":true}");
}

TEST_CASE("a chunked answer is waited for wherever it stops short", "[http][framing]") {
    // Inside a size line, inside the data, before the line end after it, before the last empty
    // line: every one of them is a body still arriving.
    const std::string_view head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::string whole = std::string(head) + "5\r\nhello\r\n0\r\n\r\n";

    everyCutIsShort(whole, head.size());
    CHECK(parseHttpResponse(whole, false).state == State::Complete);
}

TEST_CASE("a chunk size that is not hex is not an answer", "[http][framing]") {
    CHECK(parseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                            "zz\r\nhello\r\n0\r\n\r\n",
                            false)
              .state == State::Malformed);
}

TEST_CASE("a chunk that runs past its size is not an answer", "[http][framing]") {
    CHECK(parseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                            "3\r\nhello\r\n0\r\n\r\n",
                            false)
              .state == State::Malformed);
}

TEST_CASE("chunked framing outranks a declared length, in any case", "[http][framing]") {
    const HttpParse parse = parseHttpResponse("HTTP/1.1 200 OK\r\n"
                                              "Content-Length: 99\r\n"
                                              "Transfer-Encoding: Chunked\r\n"
                                              "\r\n"
                                              "2\r\nok\r\n0\r\n\r\n",
                                              false);

    REQUIRE(parse.state == State::Complete);
    CHECK(parse.response.body == "ok");
}

TEST_CASE("a body with neither a length nor chunks ends when the peer hangs up",
          "[http][framing]") {
    const std::string_view unsized = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\n\r\n<root/>";

    CHECK(parseHttpResponse(unsized, false).state == State::Incomplete);
    const HttpParse parse = parseHttpResponse(unsized, true);
    REQUIRE(parse.state == State::Complete);
    CHECK(parse.response.body == "<root/>");
}

TEST_CASE("a 204 and a 304 end at their head", "[http][framing]") {
    // Neither may carry a body, so neither waits for a length or a hang-up it will never get.
    for (const std::string_view statusLine :
         {"HTTP/1.1 204 No Content", "HTTP/1.1 304 Not Modified"}) {
        INFO(statusLine);
        const HttpParse parse =
            parseHttpResponse(std::string(statusLine) + "\r\nETag: \"v7\"\r\n\r\n", false);
        REQUIRE(parse.state == State::Complete);
        CHECK(parse.response.body.empty());
        CHECK(parse.response.header("etag") == "\"v7\"");
    }
}

TEST_CASE("an interim 1xx answer is passed over for the final one", "[http][framing]") {
    const HttpParse parse = parseHttpResponse("HTTP/1.1 100 Continue\r\n\r\n"
                                              "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",
                                              false);

    REQUIRE(parse.state == State::Complete);
    CHECK(parse.response.status == 201);
    CHECK(parse.response.body == "ok");
}

TEST_CASE("a header is found by its name in any case, its value trimmed", "[http][framing]") {
    const HttpParse parse = parseHttpResponse("HTTP/1.1 200 OK\r\n"
                                              "ETAG:  \"v7\"\t\r\n"
                                              "X-Empty:\r\n"
                                              "content-LENGTH: 2\r\n"
                                              "\r\n"
                                              "ok",
                                              false);

    REQUIRE(parse.state == State::Complete);
    CHECK(parse.response.header("etag") == "\"v7\"");
    CHECK(parse.response.header("x-empty").empty());
    CHECK(parse.response.header("x-absent").empty());
    CHECK(parse.response.body == "ok");
}

TEST_CASE("bare LF line ends read the same as CRLF", "[http][framing]") {
    const HttpParse parse = parseHttpResponse("HTTP/1.1 200 OK\nContent-Length: 2\n\nok", false);

    REQUIRE(parse.state == State::Complete);
    CHECK(parse.response.body == "ok");
}

TEST_CASE("a head is waited for until its empty line", "[http][framing]") {
    everyCutIsShort("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0);
}

TEST_CASE("a status line may end at its code", "[http][framing]") {
    const HttpParse parse = parseHttpResponse("HTTP/1.0 404\r\n\r\n", true);

    REQUIRE(parse.state == State::Complete);
    CHECK(parse.response.status == 404);
}

TEST_CASE("what does not open with an HTTP/1.x status line is not an answer", "[http][framing]") {
    // Each is followed by a whole answer, so an opening that was merely passed over, as an
    // interim one is, would read as that answer instead.
    const std::string_view openings[] = {
        "HTTP/2 200 OK",   "RTSP/1.0 200 OK",  "HTTP/1.10 200 OK", "HTTP/1.1 20",
        "HTTP/1.1 2x0 OK", "HTTP/1.1 099 Low", "HTTP/1.1 2000 OK", "HTTP/1.1  200 OK",
    };
    for (const std::string_view opening : openings) {
        INFO(opening);
        const std::string bytes =
            std::string(opening) + "\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        CHECK(parseHttpResponse(bytes, false).state == State::Malformed);
    }
}

TEST_CASE("a header line with no colon is not an answer", "[http][framing]") {
    CHECK(parseHttpResponse("HTTP/1.1 200 OK\r\nno colon here\r\nContent-Length: 0\r\n\r\n", false)
              .state == State::Malformed);
}
