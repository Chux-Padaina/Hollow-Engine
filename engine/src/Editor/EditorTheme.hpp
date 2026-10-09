// The editor's look: palette (dark/light greys, neon lime, black and white),
// fonts, and the custom widgets the panels are built from: pill buttons,
// toggle switches, icon buttons with hand-drawn icons, chips, animated
// collapsible section cards and labeled property rows.
#pragma once

#include <imgui.h>

#include <string>

namespace Hollow::theme {

// --- Palette -------------------------------------------------------------------
namespace color {
inline const ImVec4 kBackground{0.071f, 0.071f, 0.075f, 1.0f}; // #121213
inline const ImVec4 kPanel{0.118f, 0.118f, 0.125f, 1.0f};      // #1e1e20
inline const ImVec4 kRaised{0.165f, 0.165f, 0.176f, 1.0f};     // #2a2a2d
inline const ImVec4 kHover{0.204f, 0.204f, 0.216f, 1.0f};      // #343437
inline const ImVec4 kBorder{0.184f, 0.184f, 0.196f, 1.0f};     // #2f2f32
inline const ImVec4 kWell{0.051f, 0.051f, 0.055f, 1.0f};       // inputs
inline const ImVec4 kText{0.945f, 0.945f, 0.945f, 1.0f};
inline const ImVec4 kMuted{0.545f, 0.545f, 0.565f, 1.0f};
inline const ImVec4 kDim{0.36f, 0.36f, 0.38f, 1.0f};
inline const ImVec4 kLime{0.831f, 1.0f, 0.247f, 1.0f};         // #d4ff3f
inline const ImVec4 kLimeDim{0.62f, 0.76f, 0.17f, 1.0f};
inline const ImVec4 kLimeSoft{0.831f, 1.0f, 0.247f, 0.13f};
inline const ImVec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
inline const ImVec4 kBlack{0.02f, 0.02f, 0.02f, 1.0f};
inline const ImVec4 kWarning{1.0f, 0.78f, 0.35f, 1.0f};
inline const ImVec4 kError{1.0f, 0.42f, 0.4f, 1.0f};
inline const ImVec4 kAxisX{0.93f, 0.33f, 0.36f, 1.0f};
inline const ImVec4 kAxisY{0.55f, 0.85f, 0.25f, 1.0f};
inline const ImVec4 kAxisZ{0.33f, 0.58f, 0.98f, 1.0f};
} // namespace color

ImU32 u32(const ImVec4& c, float alpha_scale = 1.0f);

// --- Setup -------------------------------------------------------------------------
// Loads Segoe UI (+ semibold for headings) rasterized at 2x.
void load_fonts();
// Applies style metrics for the UI scale (call every frame).
void apply_style(float scale);
float scale();
ImFont* heading_font();
ImFont* body_font();
// Advances animation clocks; call once per frame.
void new_frame(float delta_seconds);

// --- Icons -----------------------------------------------------------------------
enum class Icon {
    Select, Move, Rotate, Scale, World, Local, Magnet, Grid,
    Play, Pause, Stop, Step,
    Plus, Trash, Copy, Eye, EyeOff, Chevron, ChevronDown, Close,
    Cube, Sphere, Plane, Light, Sun, Spot, Camera, Folder, Empty,
    Gear, Layers, Chart, Terminal, Probe, Volume, Decal, Particles, Terrain, Water, Bone,
    Search, Save, Open, Import, Undo, Redo, Bake, Sparkle, Physics, Script, Collider,
};
void draw_icon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color);
// The Hollow Engine logo (same artwork as assets/branding/hollow_logo.svg):
// a chamfered black badge with a broken lime ring cut by a slash.
void draw_logo(ImDrawList* dl, ImVec2 top_left, float size);

// --- Widgets -----------------------------------------------------------------------
enum class Pill {
    Primary,   // lime, black text
    Secondary, // raised grey
    Ghost,     // transparent until hovered
    White,     // white, black text
    Danger,
};
bool pill_button(const char* label, Pill style = Pill::Secondary, ImVec2 size = {0, 0}, Icon icon = Icon::Empty, bool has_icon = false);
// Square rounded button with an icon. `active` = lime highlight.
bool icon_button(const char* id, Icon icon, bool active = false, const char* tooltip = nullptr, float size = 0.0f);
// iOS-style switch. Returns true when toggled.
bool toggle(const char* id, bool* value);
// Small rounded tag.
void chip(const char* text, const ImVec4& fg = color::kMuted, const ImVec4& bg = color::kRaised);
// Muted uppercase-ish caption.
void caption(const char* text);
// A thin separator line with spacing.
void divider();
// Lime dot + title (+ optional muted subtitle) for card headers.
void title(const char* text, const char* subtitle = nullptr);

// Animated collapsible card. Draw contents only when it returns true, then
// call end_section() (always, even when it returns false).
bool begin_section(const char* id, const char* label, bool default_open = true, const char* subtitle = nullptr, Icon icon = Icon::Empty, bool has_icon = false);
// Same, with a small icon button at the right of the header; sets
// *action_clicked when it's pressed.
bool begin_section_action(const char* id, const char* label, bool default_open, Icon icon, Icon action_icon, const char* action_tooltip, bool* action_clicked);
void end_section();
// Nested, lighter collapsible header (no card).
bool begin_subsection(const char* id, const char* label, bool default_open = false);
void end_subsection();

// Two-column property rows: muted label on the left, widget on the right.
// begin_row returns the width available for the widget.
float begin_row(const char* label, const char* tooltip = nullptr);
void end_row();
float label_column_width();

// Segmented control (tabs). Returns true when the selection changed.
// Sliders drawn as a filled track (lime fill up to the value, thin marker)
// so the value text is never covered by a grab handle.
bool slider_float(const char* id, float* value, float min, float max, const char* format, bool logarithmic = false);
bool slider_int(const char* id, int* value, int min, int max);

bool segmented(const char* id, int* current, const char* const* labels, int count, float width = -1.0f);

// Animated value (0..1) toward `target` for the given id.
float animate(ImGuiID id, bool target, float speed = 10.0f);

} // namespace Hollow::theme
