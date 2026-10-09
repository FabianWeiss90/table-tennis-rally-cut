// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ttrally::io {

/// Malformed JSON, or a value of another type than expected.
class JsonError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// A parsed JSON value. Small documents only (manifests, model metadata); numbers are doubles.
class JsonValue {
  public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;

    JsonValue() = default; ///< null
    explicit JsonValue(bool value) : value_(value) {}
    explicit JsonValue(double value) : value_(value) {}
    explicit JsonValue(std::string value) : value_(std::move(value)) {}
    explicit JsonValue(Array value) : value_(std::make_shared<Array>(std::move(value))) {}
    explicit JsonValue(Object value) : value_(std::make_shared<Object>(std::move(value))) {}

    [[nodiscard]] bool is_null() const noexcept {
        return std::holds_alternative<std::monostate>(value_);
    }
    // The accessors throw JsonError for a value of another type.
    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] double as_number() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] const Object& as_object() const;

    /// Member of an object; throws JsonError if it is missing.
    [[nodiscard]] const JsonValue& at(std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const;

  private:
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>,
                 std::shared_ptr<Object>>
        value_;
};

/// Parses a complete JSON document. Throws JsonError.
[[nodiscard]] JsonValue parse_json(std::string_view text);

} // namespace ttrally::io
