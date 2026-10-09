// Snapshot-based undo/redo. Before an edit, the editor records the scene's
// serialized state (SceneSerializer, absolute paths, no vertex data — meshes
// are referenced by source and shared through the AssetManager, so a
// snapshot is small). Undo swaps the current state for the previous one.
// Whole-scene snapshots are simpler and far less bug-prone than per-command
// inverse operations; they're cheap at the scene sizes this engine targets.
#pragma once

#include "Hollow/Core/Json.hpp"

#include <cstddef>
#include <deque>
#include <optional>

namespace Hollow {

class UndoStack {
public:
    explicit UndoStack(std::size_t capacity = 128) : capacity_(capacity) {}

    // Records the state *before* an edit. Clears the redo history.
    void record(JsonValue before);
    // Returns the state to restore; `current` is kept for redo.
    std::optional<JsonValue> undo(JsonValue current);
    std::optional<JsonValue> redo(JsonValue current);
    void clear();

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    std::size_t undo_count() const { return undo_.size(); }

private:
    std::size_t capacity_;
    std::deque<JsonValue> undo_;
    std::deque<JsonValue> redo_;
};

} // namespace Hollow
