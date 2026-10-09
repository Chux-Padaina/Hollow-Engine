// Minimal JSON value type with a recursive-descent parser and a writer.
// Used by the glTF importer and by scene save/load + undo snapshots. Object
// keys are kept sorted (std::map), which makes written files diff-friendly.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace Hollow {

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

    JsonValue() = default;
    JsonValue(bool value);
    JsonValue(int value);
    JsonValue(unsigned int value);
    JsonValue(float value);
    JsonValue(double value);
    JsonValue(const char* value);
    JsonValue(std::string value);

    static JsonValue array();
    static JsonValue object();

    bool is_null() const { return kind == Kind::Null; }
    bool is_bool() const { return kind == Kind::Bool; }
    bool is_number() const { return kind == Kind::Number; }
    bool is_string() const { return kind == Kind::String; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_object() const { return kind == Kind::Object; }

    // Lookups never throw: a missing key/index yields a shared null value.
    const JsonValue& at(const std::string& key) const;
    const JsonValue& at(std::size_t index) const;
    const JsonValue* find(const std::string& key) const;
    std::size_t size() const;

    int as_int(int fallback = 0) const;
    float as_float(float fallback = 0.0f) const;
    bool as_bool(bool fallback = false) const;
    std::string as_string(std::string fallback = {}) const;

    // Builders. set() converts a null value into an object, push() into an
    // array.
    JsonValue& set(const std::string& key, JsonValue value);
    JsonValue& push(JsonValue value);
};

// Throws std::runtime_error (with a byte offset) on malformed input.
JsonValue parse_json(const std::string& text);
std::string write_json(const JsonValue& value, bool pretty = true);

} // namespace Hollow
