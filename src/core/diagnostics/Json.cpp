// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/diagnostics/Json.h"

#include "core/Bits.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace dish::diagnostics {

namespace {

constexpr std::size_t kIndentWidth = 2;

void indent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * kIndentWidth, ' ');
}

// The two-character escapes JSON names; every other control character is \u00XX.
char shortEscapeFor(char ch) {
    switch (ch) {
    case '"':
        return '"';
    case '\\':
        return '\\';
    case '\b':
        return 'b';
    case '\f':
        return 'f';
    case '\n':
        return 'n';
    case '\r':
        return 'r';
    case '\t':
        return 't';
    default:
        return '\0';
    }
}

void appendEscaped(std::string& out, char ch) {
    constexpr std::string_view kHex = "0123456789abcdef";
    constexpr unsigned char kFirstPrintable = 0x20;
    const char shortEscape = shortEscapeFor(ch);
    if (shortEscape != '\0') {
        out.push_back('\\');
        out.push_back(shortEscape);
        return;
    }
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < kFirstPrintable) {
        out += "\\u00";
        out.push_back(kHex[bits::highNibble(byte)]);
        out.push_back(kHex[bits::lowNibble(byte)]);
        return;
    }
    out.push_back(ch);
}

// Thousandths in integer arithmetic, so no C locale (which can make the point a comma) and no
// printf rounding mode reaches the file; trailing zeros and a bare point are dropped.
std::string fractionText(double value) {
    if (!std::isfinite(value)) { return "null"; }
    constexpr std::int64_t kScale = 1000;
    constexpr int kPlaces = 3;
    const auto scaled =
        static_cast<std::int64_t>(std::llround(value * static_cast<double>(kScale)));
    const bool negative = scaled < 0;
    const std::int64_t magnitude = negative ? -scaled : scaled;
    std::string fraction = std::to_string(magnitude % kScale);
    fraction.insert(fraction.begin(), static_cast<std::size_t>(kPlaces) - fraction.size(), '0');
    while (!fraction.empty() && fraction.back() == '0') { fraction.pop_back(); }
    std::string text = negative ? "-" : "";
    text += std::to_string(magnitude / kScale);
    if (!fraction.empty()) {
        text.push_back('.');
        text += fraction;
    }
    return text;
}

} // namespace

std::string jsonQuoted(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char ch : text) { appendEscaped(out, ch); }
    out.push_back('"');
    return out;
}

Json& Json::push(Json value) {
    items_.push_back(std::move(value));
    return *this;
}

Json& Json::set(std::string_view key, Json value) {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            items_[i] = std::move(value);
            return *this;
        }
    }
    keys_.emplace_back(key);
    items_.push_back(std::move(value));
    return *this;
}

const Json* Json::find(std::string_view key) const {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) { return &items_[i]; }
    }
    return nullptr;
}

std::string Json::dump() const {
    std::string out;
    write(out, 0);
    out.push_back('\n');
    return out;
}

// One arm per kind; the two containers share the bracket-and-indent walk, an object adding the
// key before each value.
void Json::write(std::string& out, int depth) const {
    switch (kind_) {
    case Kind::Null:
        out += "null";
        return;
    case Kind::Bool:
        out += bool_ ? "true" : "false";
        return;
    case Kind::Integer:
        out += std::to_string(integer_);
        return;
    case Kind::Double:
        out += fractionText(double_);
        return;
    case Kind::String:
        out += jsonQuoted(string_);
        return;
    case Kind::Array:
    case Kind::Object:
        break;
    }
    const bool isObject = kind_ == Kind::Object;
    const char open = isObject ? '{' : '[';
    const char close = isObject ? '}' : ']';
    if (items_.empty()) {
        out.push_back(open);
        out.push_back(close);
        return;
    }
    out.push_back(open);
    out.push_back('\n');
    for (std::size_t i = 0; i < items_.size(); ++i) {
        indent(out, depth + 1);
        if (isObject) {
            out += jsonQuoted(keys_[i]);
            out += ": ";
        }
        items_[i].write(out, depth + 1);
        const bool last = i + 1 == items_.size();
        if (!last) { out.push_back(','); }
        out.push_back('\n');
    }
    indent(out, depth);
    out.push_back(close);
}

} // namespace dish::diagnostics
