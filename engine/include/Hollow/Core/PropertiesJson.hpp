// JSON serialization through PropertyVisitor (see Properties.hpp): a type's
// describe() function is all that's needed to save and load it.
#pragma once

#include "Hollow/Core/Json.hpp"
#include "Hollow/Core/Properties.hpp"

#include <filesystem>
#include <memory>

namespace Hollow {

// Writes every visited field into a JSON object. Groups with a key become
// nested objects. Paths are stored relative to `base_dir` (when not empty).
class JsonWriteVisitor final : public PropertyVisitor {
public:
    explicit JsonWriteVisitor(JsonValue& root, std::filesystem::path base_dir = {});
    ~JsonWriteVisitor() override;

    bool begin_group(const char* key, const char* label) override;
    void end_group() override;
    void boolean(const char* key, const char* label, bool& value, const char* tip) override;
    void number(const char* key, const char* label, float& value, NumberRange range, const char* tip) override;
    void integer(const char* key, const char* label, int& value, int min_value, int max_value, const char* tip) override;
    void vec3(const char* key, const char* label, Vec3& value, NumberRange range, const char* tip) override;
    void color(const char* key, const char* label, Vec3& value, bool hdr, const char* tip) override;
    void angle(const char* key, const char* label, float& radians, float min_degrees, float max_degrees, const char* tip) override;
    void choice(const char* key, const char* label, int& value, std::span<const char* const> names, const char* tip) override;
    void text(const char* key, const char* label, std::string& value, const char* tip) override;
    void path(const char* key, const char* label, std::string& value, const char* filter, const char* tip) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Reads fields that are present; missing or mistyped fields keep their
// current value, so old files load with defaults for new fields.
class JsonReadVisitor final : public PropertyVisitor {
public:
    explicit JsonReadVisitor(const JsonValue& root, std::filesystem::path base_dir = {});
    ~JsonReadVisitor() override;

    bool begin_group(const char* key, const char* label) override;
    void end_group() override;
    void boolean(const char* key, const char* label, bool& value, const char* tip) override;
    void number(const char* key, const char* label, float& value, NumberRange range, const char* tip) override;
    void integer(const char* key, const char* label, int& value, int min_value, int max_value, const char* tip) override;
    void vec3(const char* key, const char* label, Vec3& value, NumberRange range, const char* tip) override;
    void color(const char* key, const char* label, Vec3& value, bool hdr, const char* tip) override;
    void angle(const char* key, const char* label, float& radians, float min_degrees, float max_degrees, const char* tip) override;
    void choice(const char* key, const char* label, int& value, std::span<const char* const> names, const char* tip) override;
    void text(const char* key, const char* label, std::string& value, const char* tip) override;
    void path(const char* key, const char* label, std::string& value, const char* filter, const char* tip) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

template <typename T>
JsonValue properties_to_json(const T& object, const std::filesystem::path& base_dir = {}) {
    JsonValue json = JsonValue::object();
    JsonWriteVisitor writer(json, base_dir);
    describe(const_cast<T&>(object), writer); // the writer never modifies
    return json;
}

template <typename T>
void properties_from_json(T& object, const JsonValue& json, const std::filesystem::path& base_dir = {}) {
    JsonReadVisitor reader(json, base_dir);
    describe(object, reader);
}

} // namespace Hollow
