// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The smallest JSON writer the diagnostics bundle needs: null, booleans,
// integers, fractions, strings, arrays and objects whose keys keep the order
// they were set in, printed with two-space indentation. Write-only and Qt-free, so the
// bundle builder stays host-testable and the Linux port can take it as is.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dish::diagnostics {

class Json {
  public:
    enum class Kind : std::uint8_t { Null, Bool, Integer, Double, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : kind_(Kind::Bool), bool_(value) {}
    Json(int value) : kind_(Kind::Integer), integer_(value) {}
    Json(std::int64_t value) : kind_(Kind::Integer), integer_(value) {}
    // Written to three decimal places, the precision a millisecond figure needs; a value that is
    // not finite is written as null, which JSON has in place of NaN.
    Json(double value) : kind_(Kind::Double), double_(value) {}
    Json(std::string value) : kind_(Kind::String), string_(std::move(value)) {}
    Json(std::string_view value) : kind_(Kind::String), string_(value) {}
    Json(const char* value) : kind_(Kind::String), string_(value) {}

    static Json array() { return Json(Kind::Array); }
    static Json object() { return Json(Kind::Object); }

    // An optional value, or null.
    template <class T> static Json orNull(const std::optional<T>& value) {
        return value.has_value() ? Json(*value) : Json();
    }

    Kind kind() const { return kind_; }

    // Appends to an array.
    Json& push(Json value);
    // Sets a key on an object; a key set twice keeps its first place and its last value.
    Json& set(std::string_view key, Json value);

    // Read-back, for tests: the value under a key (null when absent), an array's items.
    const Json* find(std::string_view key) const;
    const std::vector<Json>& items() const { return items_; }
    const std::string& stringValue() const { return string_; }
    std::int64_t integerValue() const { return integer_; }
    double doubleValue() const { return double_; }
    // An object's keys, in the order they were first set.
    const std::vector<std::string>& keys() const { return keys_; }
    bool boolValue() const { return bool_; }

    std::string dump() const;

  private:
    explicit Json(Kind kind) : kind_(kind) {}
    void write(std::string& out, int depth) const;

    Kind kind_ = Kind::Null;
    bool bool_ = false;
    std::int64_t integer_ = 0;
    double double_ = 0.0;
    std::string string_;
    // An array's items, or an object's values beside their keys.
    std::vector<Json> items_;
    std::vector<std::string> keys_;
};

// A JSON string literal, quotes included.
std::string jsonQuoted(std::string_view text);

} // namespace dish::diagnostics
