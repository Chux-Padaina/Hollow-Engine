// Implementation of Input.hpp.
#include "Hollow/Core/Input.hpp"

#include <Windows.h>

#include <algorithm>

namespace Hollow {
namespace {

bool valid_key(int key) {
    return key >= 0 && key < 256;
}

std::size_t button_index(MouseButton button) {
    return static_cast<std::size_t>(button);
}

} // namespace

Input::Input() {
    set_default_bindings();
}

void Input::begin_frame() {
    pressed_.fill(false);
    released_.fill(false);
    mouse_pressed_.fill(false);
    mouse_released_.fill(false);
    mouse_delta_x_ = 0;
    mouse_delta_y_ = 0;
    wheel_delta_ = 0;
}

void Input::on_key(int virtual_key, bool down) {
    if (!valid_key(virtual_key)) {
        return;
    }
    const auto index = static_cast<std::size_t>(virtual_key);
    if (down && !down_[index]) {
        pressed_[index] = true;
    }
    if (!down && down_[index]) {
        released_[index] = true;
    }
    down_[index] = down;
}

void Input::on_mouse_move(int x, int y) {
    if (has_mouse_position_) {
        mouse_delta_x_ += x - mouse_x_;
        mouse_delta_y_ += y - mouse_y_;
    }
    mouse_x_ = x;
    mouse_y_ = y;
    has_mouse_position_ = true;
}

void Input::on_mouse_button(MouseButton button, bool down) {
    const std::size_t index = button_index(button);
    if (down && !mouse_down_[index]) {
        mouse_pressed_[index] = true;
    }
    if (!down && mouse_down_[index]) {
        mouse_released_[index] = true;
    }
    mouse_down_[index] = down;
}

void Input::on_mouse_wheel(int delta) {
    wheel_delta_ += delta;
}

void Input::on_focus_lost() {
    // Without this a key held while alt-tabbing away would stay "down"
    // forever, since its WM_KEYUP goes to another window.
    for (std::size_t i = 0; i < down_.size(); ++i) {
        if (down_[i]) {
            released_[i] = true;
        }
    }
    down_.fill(false);
    for (std::size_t i = 0; i < mouse_down_.size(); ++i) {
        if (mouse_down_[i]) {
            mouse_released_[i] = true;
        }
    }
    mouse_down_.fill(false);
}

bool Input::key_down(int virtual_key) const {
    return valid_key(virtual_key) && down_[static_cast<std::size_t>(virtual_key)];
}

bool Input::key_pressed(int virtual_key) const {
    return valid_key(virtual_key) && pressed_[static_cast<std::size_t>(virtual_key)];
}

bool Input::key_released(int virtual_key) const {
    return valid_key(virtual_key) && released_[static_cast<std::size_t>(virtual_key)];
}

// Modifiers also count when pressed this frame, so a very quick Ctrl+Z
// (both keys down and up between two frames) still registers as a chord.
bool Input::ctrl() const {
    return key_down(VK_CONTROL) || key_pressed(VK_CONTROL);
}

bool Input::shift() const {
    return key_down(VK_SHIFT) || key_pressed(VK_SHIFT);
}

bool Input::alt() const {
    return key_down(VK_MENU) || key_pressed(VK_MENU);
}

bool Input::mouse_down(MouseButton button) const {
    return mouse_down_[button_index(button)];
}

bool Input::mouse_pressed(MouseButton button) const {
    return mouse_pressed_[button_index(button)];
}

bool Input::mouse_released(MouseButton button) const {
    return mouse_released_[button_index(button)];
}

void Input::bind_action(const std::string& name, int virtual_key) {
    actions_[name].push_back(virtual_key);
}

void Input::bind_axis(const std::string& name, int positive_key, int negative_key) {
    axes_[name].push_back({positive_key, negative_key});
}

void Input::clear_bindings() {
    actions_.clear();
    axes_.clear();
}

bool Input::action_down(const std::string& name) const {
    auto it = actions_.find(name);
    if (it == actions_.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [this](int key) { return key_down(key); });
}

bool Input::action_pressed(const std::string& name) const {
    auto it = actions_.find(name);
    if (it == actions_.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [this](int key) { return key_pressed(key); });
}

float Input::axis(const std::string& name) const {
    auto it = axes_.find(name);
    if (it == axes_.end()) {
        return 0.0f;
    }
    float value = 0.0f;
    for (const AxisBinding& binding : it->second) {
        if (key_down(binding.positive)) value += 1.0f;
        if (key_down(binding.negative)) value -= 1.0f;
    }
    return std::clamp(value, -1.0f, 1.0f);
}

void Input::set_default_bindings() {
    clear_bindings();
    bind_axis("MoveX", 'D', 'A');
    bind_axis("MoveX", VK_RIGHT, VK_LEFT);
    bind_axis("MoveZ", 'W', 'S');
    bind_axis("MoveZ", VK_UP, VK_DOWN);
    bind_action("Jump", VK_SPACE);
    bind_action("Sprint", VK_SHIFT);
}

} // namespace Hollow
