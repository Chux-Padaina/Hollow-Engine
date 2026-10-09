// A small reflection layer: a type describes its editable fields once, by
// calling a PropertyVisitor, and that single description drives the scene
// serializer (JSON read/write), the editor inspector and the render
// settings panel. Adding a field means adding one line to the type's
// describe() function instead of touching three files.
//
//   void describe(FogSettings& fog, PropertyVisitor& v) {
//       v.boolean("enabled", "Enabled", fog.enabled);
//       if (v.show(fog.enabled)) {
//           v.number("density", "Density", fog.density, {0.0f, 0.5f, 0.001f});
//       }
//   }
//
// show() lets the inspector hide fields that don't apply (serializers
// always visit everything). Groups nest JSON objects when they have a key,
// and become collapsible sections in the inspector.
#pragma once

#include "Hollow/Core/Math.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace Hollow {

struct NumberRange {
    float min = 0.0f;
    float max = 0.0f;   // min == max: unbounded
    float speed = 0.01f; // drag speed (sliders ignore it)
    const char* format = "%.3f";
    bool slider = true;      // slider when bounded, drag otherwise
    bool logarithmic = false;
};

class PropertyVisitor {
public:
    virtual ~PropertyVisitor() = default;

    // Return false to skip a group's contents (a collapsed UI section).
    virtual bool begin_group(const char* key, const char* label) {
        (void)key;
        (void)label;
        return true;
    }
    virtual void end_group() {}
    // Inspector: only shows the following fields when `condition` holds.
    // Serializers always return true.
    virtual bool show(bool condition) {
        (void)condition;
        return true;
    }
    // Read-only explanatory text (inspector only).
    virtual void note(const char* text) { (void)text; }

    virtual void boolean(const char* key, const char* label, bool& value, const char* tip = nullptr) = 0;
    virtual void number(const char* key, const char* label, float& value, NumberRange range = {}, const char* tip = nullptr) = 0;
    virtual void integer(const char* key, const char* label, int& value, int min_value, int max_value, const char* tip = nullptr) = 0;
    virtual void vec3(const char* key, const char* label, Vec3& value, NumberRange range = {}, const char* tip = nullptr) = 0;
    virtual void color(const char* key, const char* label, Vec3& value, bool hdr = false, const char* tip = nullptr) = 0;
    // Stored in radians, edited and saved in degrees.
    virtual void angle(const char* key, const char* label, float& radians, float min_degrees, float max_degrees, const char* tip = nullptr) = 0;
    virtual void choice(const char* key, const char* label, int& value, std::span<const char* const> names, const char* tip = nullptr) = 0;
    virtual void text(const char* key, const char* label, std::string& value, const char* tip = nullptr) = 0;
    // A UTF-8 file path. Serializers store it relative to the scene file.
    // `filter` is a description for the open dialog, e.g. "Images".
    virtual void path(const char* key, const char* label, std::string& value, const char* filter, const char* tip = nullptr) = 0;

    template <typename Enum>
    void enumeration(const char* key, const char* label, Enum& value, std::span<const char* const> names, const char* tip = nullptr) {
        int index = static_cast<int>(value);
        choice(key, label, index, names, tip);
        value = static_cast<Enum>(index);
    }
};

// RAII group helper: `if (PropertyGroup g{v, "fog", "Fog"}) { ... }`.
class PropertyGroup {
public:
    PropertyGroup(PropertyVisitor& visitor, const char* key, const char* label)
        : visitor_(visitor), open_(visitor.begin_group(key, label)) {}
    ~PropertyGroup() {
        visitor_.end_group();
    }
    PropertyGroup(const PropertyGroup&) = delete;
    PropertyGroup& operator=(const PropertyGroup&) = delete;
    explicit operator bool() const { return open_; }

private:
    PropertyVisitor& visitor_;
    bool open_;
};

} // namespace Hollow
