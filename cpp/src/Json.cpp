#include "Forge/Json.hpp"

#include <cctype>
#include <charconv>
#include <stdexcept>

namespace Forge {
namespace {

const JsonValue kNullValue{};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    JsonValue parse() {
        JsonValue value = parse_value();
        skip_ws();
        if (pos_ != text_.size()) {
            fail("unexpected text after JSON value");
        }
        return value;
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;

    void fail(const char* message) const {
        throw std::runtime_error(message);
    }

    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    char take() {
        return pos_ < text_.size() ? text_[pos_++] : '\0';
    }

    void skip_ws() {
        while (std::isspace(static_cast<unsigned char>(peek())) != 0) {
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
            fail("unexpected JSON character");
        }
    }

    JsonValue parse_value() {
        skip_ws();
        const char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string_value();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) return parse_number();
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            JsonValue value;
            value.kind = JsonValue::Kind::Bool;
            value.bool_value = true;
            return value;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            JsonValue value;
            value.kind = JsonValue::Kind::Bool;
            return value;
        }
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return {};
        }
        fail("invalid JSON value");
        return {};
    }

    JsonValue parse_object() {
        JsonValue value;
        value.kind = JsonValue::Kind::Object;
        expect('{');
        if (consume('}')) {
            return value;
        }
        for (;;) {
            skip_ws();
            if (peek() != '"') {
                fail("expected JSON object key");
            }
            std::string key = parse_string();
            expect(':');
            value.object_value.emplace(std::move(key), parse_value());
            if (consume('}')) {
                break;
            }
            expect(',');
        }
        return value;
    }

    JsonValue parse_array() {
        JsonValue value;
        value.kind = JsonValue::Kind::Array;
        expect('[');
        if (consume(']')) {
            return value;
        }
        for (;;) {
            value.array_value.push_back(parse_value());
            if (consume(']')) {
                break;
            }
            expect(',');
        }
        return value;
    }

    JsonValue parse_string_value() {
        JsonValue value;
        value.kind = JsonValue::Kind::String;
        value.string_value = parse_string();
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        for (;;) {
            const char c = take();
            if (c == '\0') {
                fail("unterminated JSON string");
            }
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
            case 'u':
                pos_ += 4;
                out.push_back('?');
                break;
            default:
                fail("invalid JSON escape");
            }
        }
        return out;
    }

    JsonValue parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (std::isdigit(static_cast<unsigned char>(peek())) != 0) ++pos_;
        if (peek() == '.') {
            ++pos_;
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) ++pos_;
        }

        JsonValue value;
        value.kind = JsonValue::Kind::Number;
        const std::string number = text_.substr(start, pos_ - start);
        value.number_value = std::stod(number);
        return value;
    }
};

} // namespace

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

JsonValue parse_json(const std::string& text) {
    return JsonParser(text).parse();
}

} // namespace Forge
