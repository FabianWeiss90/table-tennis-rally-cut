// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "shared/io/json.hpp"

#include <cctype>
#include <charconv>
#include <format>

namespace ttrally::io {

namespace {

/// Recursive-descent parser over the document text.
class Parser {
  public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonValue document() {
        JsonValue value = parse_value();
        skip_space();
        if (position_ != text_.size()) {
            fail("unexpected text after the value");
        }
        return value;
    }

  private:
    static constexpr int kMaxDepth = 64;

    [[noreturn]] void fail(std::string_view what) const {
        throw JsonError(std::format("invalid JSON at offset {}: {}", position_, what));
    }

    void skip_space() {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    [[nodiscard]] char peek() {
        skip_space();
        if (position_ >= text_.size()) {
            fail("unexpected end");
        }
        return text_[position_];
    }

    void expect(char c) {
        if (peek() != c) {
            fail(std::format("expected '{}'", c));
        }
        ++position_;
    }

    bool consume_word(std::string_view word) {
        if (text_.substr(position_, word.size()) == word) {
            position_ += word.size();
            return true;
        }
        return false;
    }

    JsonValue parse_value() {
        if (++depth_ > kMaxDepth) {
            fail("nested too deeply");
        }
        JsonValue value;
        switch (peek()) {
        case '{':
            value = parse_object();
            break;
        case '[':
            value = parse_array();
            break;
        case '"':
            value = JsonValue(parse_string());
            break;
        default:
            if (consume_word("true")) {
                value = JsonValue(true);
            } else if (consume_word("false")) {
                value = JsonValue(false);
            } else if (!consume_word("null")) {
                value = JsonValue(parse_number());
            }
        }
        --depth_;
        return value;
    }

    JsonValue parse_object() {
        expect('{');
        JsonValue::Object object;
        if (peek() == '}') {
            ++position_;
            return JsonValue(std::move(object));
        }
        for (;;) {
            if (peek() != '"') {
                fail("expected a member name");
            }
            std::string key = parse_string();
            expect(':');
            object.insert_or_assign(std::move(key), parse_value());
            if (peek() == '}') {
                ++position_;
                return JsonValue(std::move(object));
            }
            expect(',');
        }
    }

    JsonValue parse_array() {
        expect('[');
        JsonValue::Array array;
        if (peek() == ']') {
            ++position_;
            return JsonValue(std::move(array));
        }
        for (;;) {
            array.push_back(parse_value());
            if (peek() == ']') {
                ++position_;
                return JsonValue(std::move(array));
            }
            expect(',');
        }
    }

    std::string parse_string() {
        expect('"');
        std::string result;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                return result;
            }
            if (c != '\\') {
                result += c;
                continue;
            }
            if (position_ >= text_.size()) {
                break;
            }
            result += unescape(text_[position_++]);
        }
        fail("unterminated string");
    }

    /// Escape sequences other than \uXXXX (not needed for the documents read here).
    char unescape(char c) {
        switch (c) {
        case '"':
        case '\\':
        case '/':
            return c;
        case 'b':
            return '\b';
        case 'f':
            return '\f';
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        default:
            fail("unsupported escape sequence");
        }
    }

    double parse_number() {
        const std::size_t start = position_;
        while (position_ < text_.size() &&
               std::string_view("+-0123456789.eE").find(text_[position_]) !=
                   std::string_view::npos) {
            ++position_;
        }
        double value = 0.0;
        const char* first = text_.data() + start;
        const char* last = text_.data() + position_;
        const auto [end, error] = std::from_chars(first, last, value);
        if (start == position_ || error != std::errc{} || end != last) {
            fail("expected a value");
        }
        return value;
    }

    std::string_view text_;
    std::size_t position_ = 0;
    int depth_ = 0;
};

[[noreturn]] void wrong_type(std::string_view expected) {
    throw JsonError(std::format("JSON value is not {}", expected));
}

} // namespace

bool JsonValue::as_bool() const {
    if (const auto* value = std::get_if<bool>(&value_)) {
        return *value;
    }
    wrong_type("a boolean");
}

double JsonValue::as_number() const {
    if (const auto* value = std::get_if<double>(&value_)) {
        return *value;
    }
    wrong_type("a number");
}

const std::string& JsonValue::as_string() const {
    if (const auto* value = std::get_if<std::string>(&value_)) {
        return *value;
    }
    wrong_type("a string");
}

const JsonValue::Array& JsonValue::as_array() const {
    if (const auto* value = std::get_if<std::shared_ptr<Array>>(&value_)) {
        return **value;
    }
    wrong_type("an array");
}

const JsonValue::Object& JsonValue::as_object() const {
    if (const auto* value = std::get_if<std::shared_ptr<Object>>(&value_)) {
        return **value;
    }
    wrong_type("an object");
}

const JsonValue& JsonValue::at(std::string_view key) const {
    const auto& object = as_object();
    const auto it = object.find(key);
    if (it == object.end()) {
        throw JsonError(std::format("JSON object has no member \"{}\"", key));
    }
    return it->second;
}

bool JsonValue::contains(std::string_view key) const {
    return as_object().contains(key);
}

JsonValue parse_json(std::string_view text) { return Parser(text).document(); }

} // namespace ttrally::io
