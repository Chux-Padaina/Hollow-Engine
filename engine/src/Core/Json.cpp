// Recursive-descent JSON parser and writer backing Json.hpp. Numbers are
// parsed/printed with <charconv>, so results don't depend on the C locale.
#include "Hollow/Core/Json.hpp"

#include <charconv>
#include <cmath>
#include <stdexcept>

namespace Hollow {
namespace {

const JsonValue kNullValue{};
constexpr int kMaxDepth = 256;

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    JsonValue parse() {
        JsonValue value = parse_value(0);
        skip_ws();
        if (pos_ != text_.size()) {
            fail("unexpected text after JSON value");
        }
        return value;
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(std::string("JSON parse error at byte ") + std::to_string(pos_) + ": " + message);
    }

    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    char take() {
        return pos_ < text_.size() ? text_[pos_++] : '\0';
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos_;
        }
    }

    bool consume(char c) {
        skip_ws();
        if (peek() != c) {
            return false;
        }
        ++pos_;
        return true;
    }

    void expect(char c) {
        if (!consume(c)) {
            fail("unexpected character");
        }
    }

    JsonValue parse_value(int depth) {
        if (depth > kMaxDepth) {
            fail("nesting too deep");
        }
        skip_ws();
        const char c = peek();
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') return JsonValue(parse_string());
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return JsonValue(true);
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return JsonValue(false);
        }
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return {};
        }
        fail("invalid value");
    }

    JsonValue parse_object(int depth) {
        JsonValue value = JsonValue::object();
        expect('{');
        if (consume('}')) {
            return value;
        }
        for (;;) {
            skip_ws();
            if (peek() != '"') {
                fail("expected object key");
            }
            std::string key = parse_string();
            expect(':');
            value.object_value[std::move(key)] = parse_value(depth + 1);
            if (consume('}')) {
                break;
            }
            expect(',');
        }
        return value;
    }

    JsonValue parse_array(int depth) {
        JsonValue value = JsonValue::array();
        expect('[');
        if (consume(']')) {
            return value;
        }
        for (;;) {
            value.array_value.push_back(parse_value(depth + 1));
            if (consume(']')) {
                break;
            }
            expect(',');
        }
        return value;
    }

    unsigned parse_hex4() {
        if (pos_ + 4 > text_.size()) {
            fail("truncated \\u escape");
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid \\u escape");
        }
        return value;
    }

    static void append_utf8(std::string& out, unsigned codepoint) {
        if (codepoint < 0x80) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) {
                fail("unterminated string");
            }
            const char c = take();
            if (c == '"') {
                break;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            const char escaped = take();
            switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned codepoint = parse_hex4();
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF && text_.compare(pos_, 2, "\\u") == 0) {
                    pos_ += 2;
                    const unsigned low = parse_hex4();
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    }
                }
                append_utf8(out, codepoint);
                break;
            }
            default:
                fail("invalid escape");
            }
        }
        return out;
    }

    JsonValue parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (peek() >= '0' && peek() <= '9') ++pos_;
        if (peek() == '.') {
            ++pos_;
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        double number = 0.0;
        const auto result = std::from_chars(text_.data() + start, text_.data() + pos_, number);
        if (result.ec != std::errc() || result.ptr != text_.data() + pos_) {
            fail("invalid number");
        }
        return JsonValue(number);
    }
};

void write_string(std::string& out, const std::string& text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

void write_number(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "0";
        return;
    }
    // Values that started life as floats print as floats, so the shortest
    // round-trip form is "0.1" rather than "0.10000000149011612".
    const float as_float = static_cast<float>(value);
    char buffer[64];
    std::to_chars_result result;
    if (static_cast<double>(as_float) == value) {
        result = std::to_chars(buffer, buffer + sizeof(buffer), as_float);
    } else {
        result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    }
    out.append(buffer, result.ptr);
}

void write_value(std::string& out, const JsonValue& value, bool pretty, int indent) {
    auto newline = [&](int level) {
        if (pretty) {
            out.push_back('\n');
            out.append(static_cast<std::size_t>(level) * 2, ' ');
        }
    };
    switch (value.kind) {
    case JsonValue::Kind::Null:
        out += "null";
        break;
    case JsonValue::Kind::Bool:
        out += value.bool_value ? "true" : "false";
        break;
    case JsonValue::Kind::Number:
        write_number(out, value.number_value);
        break;
    case JsonValue::Kind::String:
        write_string(out, value.string_value);
        break;
    case JsonValue::Kind::Array: {
        if (value.array_value.empty()) {
            out += "[]";
            break;
        }
        // Short arrays of plain numbers (vectors, colors) stay on one line.
        bool inline_array = value.array_value.size() <= 4;
        for (const JsonValue& item : value.array_value) {
            inline_array = inline_array && item.is_number();
        }
        out.push_back('[');
        for (std::size_t i = 0; i < value.array_value.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
                if (inline_array && pretty) out.push_back(' ');
            }
            if (!inline_array) newline(indent + 1);
            write_value(out, value.array_value[i], pretty, indent + 1);
        }
        if (!inline_array) newline(indent);
        out.push_back(']');
        break;
    }
    case JsonValue::Kind::Object: {
        if (value.object_value.empty()) {
            out += "{}";
            break;
        }
        out.push_back('{');
        bool first = true;
        for (const auto& [key, item] : value.object_value) {
            if (!first) out.push_back(',');
            first = false;
            newline(indent + 1);
            write_string(out, key);
            out += pretty ? ": " : ":";
            write_value(out, item, pretty, indent + 1);
        }
        newline(indent);
        out.push_back('}');
        break;
    }
    }
}

} // namespace

JsonValue::JsonValue(bool value) : kind(Kind::Bool), bool_value(value) {}
JsonValue::JsonValue(int value) : kind(Kind::Number), number_value(value) {}
JsonValue::JsonValue(unsigned int value) : kind(Kind::Number), number_value(value) {}
JsonValue::JsonValue(float value) : kind(Kind::Number), number_value(value) {}
JsonValue::JsonValue(double value) : kind(Kind::Number), number_value(value) {}
JsonValue::JsonValue(const char* value) : kind(Kind::String), string_value(value) {}
JsonValue::JsonValue(std::string value) : kind(Kind::String), string_value(std::move(value)) {}

JsonValue JsonValue::array() {
    JsonValue value;
    value.kind = Kind::Array;
    return value;
}

JsonValue JsonValue::object() {
    JsonValue value;
    value.kind = Kind::Object;
    return value;
}

const JsonValue& JsonValue::at(const std::string& key) const {
    if (const JsonValue* value = find(key)) {
        return *value;
    }
    return kNullValue;
}

const JsonValue& JsonValue::at(std::size_t index) const {
    if (kind != Kind::Array || index >= array_value.size()) {
        return kNullValue;
    }
    return array_value[index];
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    auto it = object_value.find(key);
    return it == object_value.end() ? nullptr : &it->second;
}

std::size_t JsonValue::size() const {
    if (kind == Kind::Array) return array_value.size();
    if (kind == Kind::Object) return object_value.size();
    return 0;
}

int JsonValue::as_int(int fallback) const {
    return kind == Kind::Number ? static_cast<int>(number_value) : fallback;
}

float JsonValue::as_float(float fallback) const {
    return kind == Kind::Number ? static_cast<float>(number_value) : fallback;
}

bool JsonValue::as_bool(bool fallback) const {
    return kind == Kind::Bool ? bool_value : fallback;
}

std::string JsonValue::as_string(std::string fallback) const {
    return kind == Kind::String ? string_value : std::move(fallback);
}

JsonValue& JsonValue::set(const std::string& key, JsonValue value) {
    if (kind != Kind::Object) {
        *this = object();
    }
    JsonValue& slot = object_value[key];
    slot = std::move(value);
    return slot;
}

JsonValue& JsonValue::push(JsonValue value) {
    if (kind != Kind::Array) {
        *this = array();
    }
    array_value.push_back(std::move(value));
    return array_value.back();
}

JsonValue parse_json(const std::string& text) {
    return JsonParser(text).parse();
}

std::string write_json(const JsonValue& value, bool pretty) {
    std::string out;
    write_value(out, value, pretty, 0);
    if (pretty) {
        out.push_back('\n');
    }
    return out;
}

} // namespace Hollow
