#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace iron {

// Generic, document-agnostic undo/redo over opaque string snapshots. A document
// (scene, node graph, …) serializes itself to a string; this class stores the
// pre-edit snapshots and hands back the snapshot to restore to on undo/redo. It
// knows nothing about what the strings contain — instantiate one per document.
//
// Model: the undo stack holds states the user can step BACK to. The current
// state is supplied at undo/redo time (not stored), so redo is exact without a
// sentinel "present" entry.
class UndoHistory {
public:
    explicit UndoHistory(std::size_t capacity = 100);

    // Push a pre-edit snapshot as a new undo entry. Clears the redo stack.
    // Evicts the oldest entry if capacity is exceeded.
    void commit(std::string beforeSnapshot);

    bool canUndo() const;
    bool canRedo() const;

    // Given the document's CURRENT serialized state, return the snapshot to
    // restore to, and record `current` on the opposite stack. Returns nullopt
    // (no-op) when the relevant stack is empty.
    std::optional<std::string> undo(const std::string& current);
    std::optional<std::string> redo(const std::string& current);

    void clear();

private:
    std::vector<std::string> undo_;
    std::vector<std::string> redo_;
    std::size_t capacity_;
};

}  // namespace iron
