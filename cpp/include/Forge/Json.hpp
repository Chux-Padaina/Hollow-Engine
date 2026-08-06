#pragma once

#include <map>
#include <string>
#include <vector>

namespace Forge {

class JsonValue {
public:
    enum class Kind {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool bool_value = false;
    double number_value = 0.0;
    std::string string_value;
    std::vector<JsonValue> array_value;
    std::map<std::string, JsonValue> object_value;

    bool is_null() const { return kind == Kind::Null; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_object() const { return kind == Kind::Object; }

    const JsonValue& at(const std::string& key) const;
    const JsonValue& at(std::size_t index) const;
    const JsonValue* find(const std::string& key) const;

    int as_int(int fallback = 0) const;
    float as_float(float fallback = 0.0f) const;
    bool as_bool(bool fallback = false) const;
    std::string as_string(std::string fallback = {}) const;
};

JsonValue parse_json(const std::string& text);

} // namespace Forge
