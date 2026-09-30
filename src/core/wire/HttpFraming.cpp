// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/wire/HttpFraming.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <system_error>

namespace dish::wire {
namespace {

using State = HttpParse::State;

// Walks a response front to back, a line or a counted run of bytes at a time. Either read answers
// nothing while what it needs has not arrived yet. A line ends at LF, and a CR in front of the LF
// belongs to the line ending: hosts send both shapes.
class Cursor {
  public:
    explicit Cursor(std::string_view bytes) : bytes_(bytes) {}

    std::optional<std::string_view> line() {
        const std::size_t end = bytes_.find('\n', offset_);
        if (end == std::string_view::npos) { return std::nullopt; }
        std::string_view text = bytes_.substr(offset_, end - offset_);
        offset_ = end + 1;
        if (text.ends_with('\r')) { text.remove_suffix(1); }
        return text;
    }

    std::optional<std::string_view> take(std::size_t count) {
        if (bytes_.size() - offset_ < count) { return std::nullopt; }
        const std::string_view run = bytes_.substr(offset_, count);
        offset_ += count;
        return run;
    }

    std::string_view rest() const { return bytes_.substr(offset_); }

  private:
    std::string_view bytes_;
    std::size_t offset_ = 0;
};

enum class Framing : std::uint8_t { None, Chunked, Length, UntilClose };

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Optional whitespace (RFC 9110 5.6.3) is spaces and tabs.
std::string_view trimmed(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t");
    const std::size_t last = text.find_last_not_of(" \t");
    return first == std::string_view::npos ? std::string_view()
                                           : text.substr(first, last - first + 1);
}

// A count is digits in `base` and nothing else; anything more is not a count to trust.
std::optional<std::size_t> countOf(std::string_view digits, int base) {
    std::size_t count = 0;
    const auto [stop, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), count, base);
    const auto consumed = static_cast<std::size_t>(stop - digits.data());
    const bool isWholeCount = error == std::errc() && consumed == digits.size();
    return isWholeCount ? std::optional<std::size_t>(count) : std::nullopt;
}

// "HTTP/1.1 200 OK" -> 200. The version is eight bytes and the code three digits; the reason
// phrase after them is free text, and may be missing.
std::optional<int> statusOf(std::string_view line) {
    constexpr std::size_t kCodeAt = 9;
    constexpr std::size_t kCodeEnd = kCodeAt + 3;
    const bool isHttp1 = line.starts_with("HTTP/1.") && line.size() >= kCodeEnd && line[8] == ' ';
    if (!isHttp1) { return std::nullopt; }
    const std::optional<std::size_t> code = countOf(line.substr(kCodeAt, 3), 10);
    const bool codeEnds = line.size() == kCodeEnd || line[kCodeEnd] == ' ';
    const bool isStatus = code.has_value() && *code >= 100 && codeEnds;
    if (!isStatus) { return std::nullopt; }
    return static_cast<int>(*code);
}

// "Name: value" -> {"name", "value"}. A line with no colon is not a field.
std::optional<HttpHeader> fieldOf(std::string_view line) {
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) { return std::nullopt; }
    return HttpHeader{lowered(line.substr(0, colon)), std::string(trimmed(line.substr(colon + 1)))};
}

// "1a;name=value" -> 26. A chunk extension carries nothing this client reads.
std::optional<std::size_t> chunkSizeOf(std::string_view line) {
    return countOf(trimmed(line.substr(0, line.find(';'))), 16);
}

HttpParse complete(HttpResponse response) { return {State::Complete, std::move(response)}; }

HttpParse malformed() { return {State::Malformed, {}}; }

// Short of a whole response: worth waiting on while the peer is there, broken once it has gone.
HttpParse stillShort(bool peerClosed) {
    return {peerClosed ? State::Malformed : State::Incomplete, {}};
}

// The status line and the fields after it, up to the empty line that ends them. Complete means the
// head is whole; the body is read after it.
HttpParse readHead(Cursor& cursor, bool peerClosed) {
    const std::optional<std::string_view> statusLine = cursor.line();
    if (!statusLine) { return stillShort(peerClosed); }
    const std::optional<int> status = statusOf(*statusLine);
    if (!status) { return malformed(); }
    HttpParse head{State::Complete, HttpResponse{*status, {}, {}}};
    while (const std::optional<std::string_view> line = cursor.line()) {
        if (line->empty()) { return head; }
        std::optional<HttpHeader> field = fieldOf(*line);
        if (!field) { return malformed(); }
        head.response.headers.push_back(std::move(*field));
    }
    return stillShort(peerClosed);
}

// RFC 9112 6.3, in its order: a 204 or a 304 never has a body, chunked transfer coding outranks a
// declared length, and a body with neither ends when the peer closes the connection.
Framing framingOf(const HttpResponse& head) {
    if (head.status == 204 || head.status == 304) { return Framing::None; }
    if (lowered(head.header("transfer-encoding")).ends_with("chunked")) { return Framing::Chunked; }
    if (!head.header("content-length").empty()) { return Framing::Length; }
    return Framing::UntilClose;
}

// After the last chunk come trailer fields, which carry nothing this client reads, and the empty
// line that ends the message.
HttpParse readTrailers(HttpResponse response, Cursor& cursor, bool peerClosed) {
    while (const std::optional<std::string_view> line = cursor.line()) {
        if (line->empty()) { return complete(std::move(response)); }
    }
    return stillShort(peerClosed);
}

// RFC 9112 7.1: a chunk is its size in hex, that many bytes and a line end; size zero is the last.
HttpParse readChunks(HttpResponse response, Cursor& cursor, bool peerClosed) {
    while (const std::optional<std::string_view> sizeLine = cursor.line()) {
        const std::optional<std::size_t> size = chunkSizeOf(*sizeLine);
        if (!size) { return malformed(); }
        if (*size == 0) { return readTrailers(std::move(response), cursor, peerClosed); }
        const std::optional<std::string_view> data = cursor.take(*size);
        if (!data) { return stillShort(peerClosed); }
        const std::optional<std::string_view> dataEnd = cursor.line();
        if (!dataEnd) { return stillShort(peerClosed); }
        if (!dataEnd->empty()) { return malformed(); }
        response.body.append(*data);
    }
    return stillShort(peerClosed);
}

HttpParse readLength(HttpResponse response, Cursor& cursor, bool peerClosed) {
    const std::optional<std::size_t> length = countOf(response.header("content-length"), 10);
    if (!length) { return malformed(); }
    const std::optional<std::string_view> body = cursor.take(*length);
    if (!body) { return stillShort(peerClosed); }
    response.body = std::string(*body);
    return complete(std::move(response));
}

HttpParse readUntilClose(HttpResponse response, const Cursor& cursor, bool peerClosed) {
    if (!peerClosed) { return {State::Incomplete, {}}; }
    response.body = std::string(cursor.rest());
    return complete(std::move(response));
}

HttpParse readBody(HttpResponse response, Cursor& cursor, bool peerClosed) {
    switch (framingOf(response)) {
    case Framing::None:
        return complete(std::move(response));
    case Framing::Chunked:
        return readChunks(std::move(response), cursor, peerClosed);
    case Framing::Length:
        return readLength(std::move(response), cursor, peerClosed);
    case Framing::UntilClose:
        return readUntilClose(std::move(response), cursor, peerClosed);
    }
    return malformed();
}

void appendField(std::string& out, std::string_view name, std::string_view value) {
    out.append(name).append(": ").append(value).append("\r\n");
}

} // namespace

std::string HttpResponse::header(std::string_view lowerName) const {
    const auto field =
        std::find_if(headers.cbegin(), headers.cend(), [lowerName](const HttpHeader& candidate) {
            return candidate.first == lowerName;
        });
    return field == headers.cend() ? std::string() : field->second;
}

std::string formatHttpRequest(std::string_view method, std::string_view target,
                              std::string_view authority, const std::vector<HttpHeader>& headers,
                              std::string_view body) {
    std::string out;
    out.append(method).append(" ").append(target).append(" HTTP/1.1\r\n");
    appendField(out, "Host", authority);
    appendField(out, "Connection", "close");
    for (const auto& [name, value] : headers) { appendField(out, name, value); }
    if (!body.empty()) { appendField(out, "Content-Length", std::to_string(body.size())); }
    out.append("\r\n").append(body);
    return out;
}

HttpParse parseHttpResponse(std::string_view received, bool peerClosed) {
    Cursor cursor(received);
    for (;;) {
        HttpParse head = readHead(cursor, peerClosed);
        if (head.state != State::Complete) { return head; }
        const bool isInterim = head.response.status < 200;
        if (!isInterim) { return readBody(std::move(head.response), cursor, peerClosed); }
    }
}

} // namespace dish::wire
