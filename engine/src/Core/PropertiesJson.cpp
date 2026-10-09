// Implementation of PropertiesJson.hpp.
#include "Hollow/Core/PropertiesJson.hpp"

#include "Hollow/Core/FileUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace Hollow {
namespace {

JsonValue vec3_to_json(Vec3 v) {
    JsonValue array = JsonValue::array();
    array.push(v.x);
    array.push(v.y);
    array.push(v.z);
    return array;
}

bool json_to_vec3(const JsonValue& json, Vec3& out) {
    if (!json.is_array() || json.size() < 3) {
        return false;
    }
    out = {json.at(std::size_t{0}).as_float(out.x), json.at(std::size_t{1}).as_float(out.y), json.at(std::size_t{2}).as_float(out.z)};
    return true;
}

} // namespace

// --- Writer ------------------------------------------------------------------

struct JsonWriteVisitor::State {
    std::filesystem::path base_dir;
    std::vector<JsonValue*> stack;
    std::vector<bool> pushed; // whether begin_group pushed a level
    JsonValue& top() { return *stack.back(); }
};

JsonWriteVisitor::JsonWriteVisitor(JsonValue& root, std::filesystem::path base_dir) : state_(std::make_unique<State>()) {
    state_->base_dir = std::move(base_dir);
    if (!root.is_object()) {
        root = JsonValue::object();
    }
    state_->stack.push_back(&root);
}

JsonWriteVisitor::~JsonWriteVisitor() = default;

bool JsonWriteVisitor::begin_group(const char* key, const char*) {
    if (key == nullptr || key[0] == '\0') {
        state_->pushed.push_back(false);
        return true;
    }
    JsonValue& parent = state_->top();
    if (parent.find(key) == nullptr) {
        parent.set(key, JsonValue::object());
    }
    state_->stack.push_back(&parent.object_value[key]);
    state_->pushed.push_back(true);
    return true;
}

void JsonWriteVisitor::end_group() {
    if (state_->pushed.empty()) {
        return;
    }
    if (state_->pushed.back()) {
        state_->stack.pop_back();
    }
    state_->pushed.pop_back();
}

void JsonWriteVisitor::boolean(const char* key, const char*, bool& value, const char*) {
    state_->top().set(key, value);
}

void JsonWriteVisitor::number(const char* key, const char*, float& value, NumberRange, const char*) {
    state_->top().set(key, value);
}

void JsonWriteVisitor::integer(const char* key, const char*, int& value, int, int, const char*) {
    state_->top().set(key, value);
}

void JsonWriteVisitor::vec3(const char* key, const char*, Vec3& value, NumberRange, const char*) {
    state_->top().set(key, vec3_to_json(value));
}

void JsonWriteVisitor::color(const char* key, const char*, Vec3& value, bool, const char*) {
    state_->top().set(key, vec3_to_json(value));
}

void JsonWriteVisitor::angle(const char* key, const char*, float& radians, float, float, const char*) {
    state_->top().set(key, radians * kRadToDeg);
}

void JsonWriteVisitor::choice(const char* key, const char*, int& value, std::span<const char* const> names, const char*) {
    if (value >= 0 && static_cast<std::size_t>(value) < names.size()) {
        state_->top().set(key, names[static_cast<std::size_t>(value)]);
    } else {
        state_->top().set(key, value);
    }
}

void JsonWriteVisitor::text(const char* key, const char*, std::string& value, const char*) {
    state_->top().set(key, value);
}

void JsonWriteVisitor::path(const char* key, const char*, std::string& value, const char*, const char*) {
    if (value.empty()) {
        state_->top().set(key, "");
        return;
    }
    if (state_->base_dir.empty()) {
        state_->top().set(key, value);
    } else {
        state_->top().set(key, make_relative_utf8(path_from_utf8(value), state_->base_dir));
    }
}

// --- Reader ------------------------------------------------------------------

struct JsonReadVisitor::State {
    std::filesystem::path base_dir;
    std::vector<const JsonValue*> stack;
    std::vector<bool> pushed;
    const JsonValue& top() const { return *stack.back(); }
};

JsonReadVisitor::JsonReadVisitor(const JsonValue& root, std::filesystem::path base_dir) : state_(std::make_unique<State>()) {
    state_->base_dir = std::move(base_dir);
    state_->stack.push_back(&root);
}

JsonReadVisitor::~JsonReadVisitor() = default;

bool JsonReadVisitor::begin_group(const char* key, const char*) {
    if (key == nullptr || key[0] == '\0') {
        state_->pushed.push_back(false);
        return true;
    }
    state_->stack.push_back(&state_->top().at(key)); // null value if missing
    state_->pushed.push_back(true);
    return true;
}

void JsonReadVisitor::end_group() {
    if (state_->pushed.empty()) {
        return;
    }
    if (state_->pushed.back()) {
        state_->stack.pop_back();
    }
    state_->pushed.pop_back();
}

void JsonReadVisitor::boolean(const char* key, const char*, bool& value, const char*) {
    const JsonValue& json = state_->top().at(key);
    if (json.is_bool() || json.is_number()) {
        value = json.as_bool(value);
    }
}

void JsonReadVisitor::number(const char* key, const char*, float& value, NumberRange, const char*) {
    const JsonValue& json = state_->top().at(key);
    if (json.is_number()) {
        value = json.as_float(value);
    }
}

void JsonReadVisitor::integer(const char* key, const char*, int& value, int min_value, int max_value, const char*) {
    const JsonValue& json = state_->top().at(key);
    if (json.is_number()) {
        value = json.as_int(value);
        if (min_value < max_value) {
            value = std::clamp(value, min_value, max_value);
        }
    }
}

void JsonReadVisitor::vec3(const char* key, const char*, Vec3& value, NumberRange, const char*) {
    json_to_vec3(state_->top().at(key), value);
}

void JsonReadVisitor::color(const char* key, const char*, Vec3& value, bool, const char*) {
    json_to_vec3(state_->top().at(key), value);
}

void JsonReadVisitor::angle(const char* key, const char*, float& radians, float, float, const char*) {
    const JsonValue& json = state_->top().at(key);
    if (json.is_number()) {
        radians = json.as_float(radians * kRadToDeg) * kDegToRad;
    }
}

void JsonReadVisitor::choice(const char* key, const char*, int& value, std::span<const char* const> names, const char*) {
    const JsonValue& json = state_->top().at(key);
    if (json.is_string()) {
        const std::string name = json.as_string();
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (name == names[i]) {
                value = static_cast<int>(i);
                return;
            }
        }
        // Case-insensitive fallback (older files used lowercase names).
        for (std::size_t i = 0; i < names.size(); ++i) {
            std::string candidate = names[i];
            if (candidate.size() == name.size() &&
                std::equal(candidate.begin(), candidate.end(), name.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
                value = static_cast<int>(i);
                return;
            }
        }
    } else if (json.is_number()) {
        const int index = json.as_int(value);
        if (index >= 0 && static_cast<std::size_t>(index) < names.size()) {
            value = index;
        }
    }
}

void JsonReadVisitor::text(const char* key, const char*, std::string& value, const char*) {
    const JsonValue& json = state_->top().at(key);
    if (json.is_string()) {
        value = json.as_string();
    }
}

void JsonReadVisitor::path(const char* key, const char*, std::string& value, const char*, const char*) {
    const JsonValue& json = state_->top().at(key);
    if (!json.is_string()) {
        return;
    }
    const std::string stored = json.as_string();
    if (stored.empty()) {
        value.clear();
    } else if (state_->base_dir.empty()) {
        value = stored;
    } else {
        value = to_utf8(resolve_utf8(stored, state_->base_dir));
    }
}

} // namespace Hollow
