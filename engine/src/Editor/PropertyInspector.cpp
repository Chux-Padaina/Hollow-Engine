// Implementation of PropertyInspector.hpp.
#include "Editor/PropertyInspector.hpp"

#include "Editor/EditorTheme.hpp"
#include "Hollow/Core/FileUtil.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>

namespace Hollow {

InspectorVisitor::InspectorVisitor(const InspectorCallbacks& callbacks, std::uint32_t* revision, bool top_level_sections)
    : callbacks_(callbacks), revision_(revision), top_level_sections_(top_level_sections) {}

template <typename T, typename Widget>
void InspectorVisitor::continuous(T& value, Widget&& widget) {
    const T before = value;
    widget();
    if (ImGui::IsItemActivated()) {
        // The widget may already have changed the value this frame: snapshot
        // with the old value in place.
        const T now = value;
        value = before;
        if (callbacks_.begin_edit) callbacks_.begin_edit();
        value = now;
    }
    if (!(value == before)) {
        changed_ = true;
        if (revision_ != nullptr) {
            ++*revision_;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (callbacks_.end_edit) callbacks_.end_edit();
    } else if (ImGui::IsItemDeactivated()) {
        if (callbacks_.cancel_edit) callbacks_.cancel_edit();
    }
}

template <typename T>
void InspectorVisitor::discrete(T& value, T next) {
    if (value == next) {
        return;
    }
    if (callbacks_.begin_edit) callbacks_.begin_edit();
    value = std::move(next);
    if (callbacks_.end_edit) callbacks_.end_edit();
    changed_ = true;
    if (revision_ != nullptr) {
        ++*revision_;
    }
}

bool InspectorVisitor::begin_group(const char* key, const char* label) {
    if (top_level_sections_ && groups_.empty() && label != nullptr) {
        const bool open = theme::begin_section(label, label, key != nullptr && std::string(key) == "exposure");
        groups_.push_back({GroupKind::Section, open});
        return open;
    }
    if (label == nullptr) {
        groups_.push_back({GroupKind::Plain, true});
        return true;
    }
    const bool open = theme::begin_subsection(label, label, false);
    groups_.push_back({GroupKind::Subsection, open});
    return open;
}

void InspectorVisitor::end_group() {
    if (groups_.empty()) {
        return;
    }
    const GroupFrame frame = groups_.back();
    groups_.pop_back();
    if (frame.kind == GroupKind::Section) {
        theme::end_section();
    } else if (frame.kind == GroupKind::Subsection && frame.open) {
        theme::end_subsection();
    }
}

void InspectorVisitor::note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color::kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void InspectorVisitor::boolean(const char*, const char* label, bool& value, const char* tip) {
    ImGui::PushID(label);
    theme::begin_row(label, tip);
    bool next = value;
    theme::toggle("##toggle", &next);
    discrete(value, next);
    ImGui::PopID();
}

void InspectorVisitor::number(const char*, const char* label, float& value, NumberRange range, const char* tip) {
    ImGui::PushID(label);
    theme::begin_row(label, tip);
    const bool bounded = range.max > range.min;
    continuous(value, [&] {
        if (bounded && range.slider) {
            theme::slider_float("##v", &value, range.min, range.max, range.format, range.logarithmic);
        } else {
            ImGui::DragFloat("##v", &value, range.speed, range.min, range.max, range.format, range.logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
        }
    });
    if (bounded) {
        value = std::clamp(value, range.min, range.max);
    }
    ImGui::PopID();
}

void InspectorVisitor::integer(const char*, const char* label, int& value, int min_value, int max_value, const char* tip) {
    ImGui::PushID(label);
    theme::begin_row(label, tip);
    continuous(value, [&] {
        if (max_value - min_value <= 64) {
            theme::slider_int("##v", &value, min_value, max_value);
        } else {
            ImGui::DragInt("##v", &value, 1.0f, min_value, max_value);
        }
    });
    value = std::clamp(value, min_value, max_value);
    ImGui::PopID();
}

void InspectorVisitor::vec3(const char*, const char* label, Vec3& value, NumberRange range, const char* tip) {
    ImGui::PushID(label);
    const float width = theme::begin_row(label, tip);
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float component = (width - spacing * 2.0f) / 3.0f;
    const ImVec4 axis[3] = {theme::color::kAxisX, theme::color::kAxisY, theme::color::kAxisZ};
    float* fields[3] = {&value.x, &value.y, &value.z};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, spacing);
        }
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(component);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        continuous(*fields[i], [&] { ImGui::DragFloat("##c", fields[i], range.speed, range.min, range.max, range.format); });
        // Axis color bar on the left edge of each field.
        ImGui::GetWindowDrawList()->AddRectFilled({pos.x, pos.y + 4.0f}, {pos.x + 2.5f * theme::scale(), pos.y + ImGui::GetFrameHeight() - 4.0f}, theme::u32(axis[i]), 2.0f);
        ImGui::PopID();
    }
    ImGui::PopID();
}

void InspectorVisitor::color(const char*, const char* label, Vec3& value, bool hdr, const char* tip) {
    ImGui::PushID(label);
    theme::begin_row(label, tip);
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex;
    if (hdr) {
        flags = ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float;
    }
    continuous(value, [&] { ImGui::ColorEdit3("##c", &value.x, flags); });
    ImGui::PopID();
}

void InspectorVisitor::angle(const char*, const char* label, float& radians, float min_degrees, float max_degrees, const char* tip) {
    ImGui::PushID(label);
    theme::begin_row(label, tip);
    float degrees = radians * kRadToDeg;
    const float before = degrees;
    continuous(radians, [&] {
        if (theme::slider_float("##a", &degrees, min_degrees, max_degrees, "%.1f deg")) {
            radians = degrees * kDegToRad;
        }
    });
    (void)before;
    ImGui::PopID();
}

void InspectorVisitor::choice(const char*, const char* label, int& value, std::span<const char* const> names, const char* tip) {
    ImGui::PushID(label);
    theme::begin_row(label, tip);
    int next = value;
    const char* preview = next >= 0 && static_cast<std::size_t>(next) < names.size() ? names[static_cast<std::size_t>(next)] : "?";
    if (ImGui::BeginCombo("##choice", preview)) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (ImGui::Selectable(names[i], static_cast<int>(i) == next)) {
                next = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    discrete(value, next);
    ImGui::PopID();
}

void InspectorVisitor::text(const char*, const char* label, std::string& value, const char* tip) {
    ImGui::PushID(label);
    theme::begin_row(label, tip);
    continuous(value, [&] { ImGui::InputText("##t", &value); });
    ImGui::PopID();
}

void InspectorVisitor::path(const char*, const char* label, std::string& value, const char* filter, const char* tip) {
    ImGui::PushID(label);
    const float width = theme::begin_row(label, tip);
    const float button = ImGui::GetFrameHeight();
    const std::string name = value.empty() ? std::string("None") : to_utf8(path_from_utf8(value).filename());
    const float field = width - (button + ImGui::GetStyle().ItemInnerSpacing.x) * (value.empty() ? 1.0f : 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::color::kWell);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::kRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.0f, 0.5f});
    ImGui::PushStyleColor(ImGuiCol_Text, value.empty() ? theme::color::kDim : theme::color::kText);
    const bool pick = ImGui::Button(name.c_str(), {std::max(field, 30.0f), 0.0f});
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    if (!value.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", value.c_str());
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    const bool browse = theme::icon_button("##browse", theme::Icon::Folder, false, "Browse...");
    if ((pick || browse) && callbacks_.browse) {
        std::string chosen = callbacks_.browse(filter);
        if (!chosen.empty()) {
            discrete(value, chosen);
        }
    }
    if (!value.empty()) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (theme::icon_button("##clear", theme::Icon::Close, false, "Clear")) {
            discrete(value, std::string());
        }
    }
    ImGui::PopID();
}

} // namespace Hollow
