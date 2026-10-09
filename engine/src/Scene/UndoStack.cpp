// Implementation of UndoStack.hpp.
#include "Hollow/Scene/UndoStack.hpp"

namespace Hollow {

void UndoStack::record(JsonValue before) {
    undo_.push_back(std::move(before));
    while (undo_.size() > capacity_) {
        undo_.pop_front();
    }
    redo_.clear();
}

std::optional<JsonValue> UndoStack::undo(JsonValue current) {
    if (undo_.empty()) {
        return std::nullopt;
    }
    JsonValue previous = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(std::move(current));
    return previous;
}

std::optional<JsonValue> UndoStack::redo(JsonValue current) {
    if (redo_.empty()) {
        return std::nullopt;
    }
    JsonValue next = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(std::move(current));
    return next;
}

void UndoStack::clear() {
    undo_.clear();
    redo_.clear();
}

} // namespace Hollow
