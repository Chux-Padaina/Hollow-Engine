// An ImGui PropertyVisitor: draws any describe()-able object (components,
// materials, render settings) as themed two-column property rows, with
// collapsible sections for groups, and brackets every edit for undo.
#pragma once

#include "Hollow/Core/Properties.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Hollow {

struct InspectorCallbacks {
    std::function<void()> begin_edit;  // snapshot before a change
    std::function<void()> end_edit;    // commit the change
    std::function<void()> cancel_edit; // nothing changed after all
    // Opens a file dialog; returns a UTF-8 path or "".
    std::function<std::string(const char* filter_description)> browse;
};

class InspectorVisitor final : public PropertyVisitor {
public:
    // `top_level_sections`: keyed groups at depth 0 become section cards.
    InspectorVisitor(const InspectorCallbacks& callbacks, std::uint32_t* revision = nullptr, bool top_level_sections = true);

    bool begin_group(const char* key, const char* label) override;
    void end_group() override;
    bool show(bool condition) override { return condition; }
    void note(const char* text) override;

    void boolean(const char* key, const char* label, bool& value, const char* tip = nullptr) override;
    void number(const char* key, const char* label, float& value, NumberRange range = {}, const char* tip = nullptr) override;
    void integer(const char* key, const char* label, int& value, int min_value, int max_value, const char* tip = nullptr) override;
    void vec3(const char* key, const char* label, Vec3& value, NumberRange range = {}, const char* tip = nullptr) override;
    void color(const char* key, const char* label, Vec3& value, bool hdr = false, const char* tip = nullptr) override;
    void angle(const char* key, const char* label, float& radians, float min_degrees, float max_degrees, const char* tip = nullptr) override;
    void choice(const char* key, const char* label, int& value, std::span<const char* const> names, const char* tip = nullptr) override;
    void text(const char* key, const char* label, std::string& value, const char* tip = nullptr) override;
    void path(const char* key, const char* label, std::string& value, const char* filter, const char* tip = nullptr) override;

    bool changed() const { return changed_; }

private:
    // Continuous widgets: snapshot the value from before the first change.
    template <typename T, typename Widget>
    void continuous(T& value, Widget&& widget);
    // Discrete widgets: apply `next` as one undo step.
    template <typename T>
    void discrete(T& value, T next);

    const InspectorCallbacks& callbacks_;
    std::uint32_t* revision_;
    bool top_level_sections_;
    enum class GroupKind {
        Section,    // begin_section: always needs end_section
        Subsection, // begin_subsection: needs end_subsection when open
        Plain,      // unlabeled group: nothing drawn
    };
    struct GroupFrame {
        GroupKind kind;
        bool open;
    };
    std::vector<GroupFrame> groups_;
    bool changed_ = false;
};

} // namespace Hollow
