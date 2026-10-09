// Event-driven input state. Win32Window feeds it from window messages
// (WM_KEYDOWN/UP, mouse, WM_KILLFOCUS), so keys only register while the
// engine window has focus — unlike GetAsyncKeyState, which reads the global
// keyboard. Each frame begin_frame() clears the per-frame edges
// (pressed/released, mouse delta, wheel).
//
// Gameplay code should prefer named actions/axes over raw keys so bindings
// live in one place:
//   input.bind_action("Jump", VK_SPACE);
//   input.bind_axis("MoveX", 'D', 'A');
//   if (input.action_pressed("Jump")) ...
//   float x = input.axis("MoveX"); // -1..1
#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hollow {

enum class MouseButton {
    Left,
    Right,
    Middle,
};

class Input {
public:
    Input();

    void begin_frame();

    // --- fed by the platform layer ---
    void on_key(int virtual_key, bool down);
    void on_mouse_move(int x, int y);
    void on_mouse_button(MouseButton button, bool down);
    void on_mouse_wheel(int delta);
    void on_focus_lost();

    // --- keyboard (Win32 virtual-key codes, e.g. 'W', VK_SPACE) ---
    bool key_down(int virtual_key) const;
    bool key_pressed(int virtual_key) const;
    bool key_released(int virtual_key) const;
    bool ctrl() const;
    bool shift() const;
    bool alt() const;

    // --- mouse ---
    int mouse_x() const { return mouse_x_; }
    int mouse_y() const { return mouse_y_; }
    int mouse_delta_x() const { return mouse_delta_x_; }
    int mouse_delta_y() const { return mouse_delta_y_; }
    int wheel_delta() const { return wheel_delta_; }
    bool mouse_down(MouseButton button) const;
    bool mouse_pressed(MouseButton button) const;
    bool mouse_released(MouseButton button) const;

    // --- actions / axes ---
    void bind_action(const std::string& name, int virtual_key);
    void bind_axis(const std::string& name, int positive_key, int negative_key);
    void clear_bindings();
    bool action_down(const std::string& name) const;
    bool action_pressed(const std::string& name) const;
    float axis(const std::string& name) const;

    // Default gameplay bindings: MoveX (D/A, Right/Left), MoveZ (W/S,
    // Up/Down), Jump (Space), Sprint (Shift).
    void set_default_bindings();

private:
    static constexpr int kKeyCount = 256;
    std::array<bool, kKeyCount> down_{};
    std::array<bool, kKeyCount> pressed_{};
    std::array<bool, kKeyCount> released_{};

    std::array<bool, 3> mouse_down_{};
    std::array<bool, 3> mouse_pressed_{};
    std::array<bool, 3> mouse_released_{};
    int mouse_x_ = 0;
    int mouse_y_ = 0;
    int mouse_delta_x_ = 0;
    int mouse_delta_y_ = 0;
    int wheel_delta_ = 0;
    bool has_mouse_position_ = false;

    struct AxisBinding {
        int positive = 0;
        int negative = 0;
    };
    std::unordered_map<std::string, std::vector<int>> actions_;
    std::unordered_map<std::string, std::vector<AxisBinding>> axes_;
};

} // namespace Hollow
