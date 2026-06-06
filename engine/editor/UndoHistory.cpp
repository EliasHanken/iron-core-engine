#include "editor/UndoHistory.h"

#include <utility>

namespace iron {

UndoHistory::UndoHistory(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void UndoHistory::commit(std::string beforeSnapshot) {
    undo_.push_back(std::move(beforeSnapshot));
    if (undo_.size() > capacity_)
        undo_.erase(undo_.begin());   // evict the oldest
    redo_.clear();
}

bool UndoHistory::canUndo() const { return !undo_.empty(); }
bool UndoHistory::canRedo() const { return !redo_.empty(); }

std::optional<std::string> UndoHistory::undo(const std::string& current) {
    if (undo_.empty()) return std::nullopt;
    redo_.push_back(current);
    std::string snapshot = std::move(undo_.back());
    undo_.pop_back();
    return snapshot;
}

std::optional<std::string> UndoHistory::redo(const std::string& current) {
    if (redo_.empty()) return std::nullopt;
    undo_.push_back(current);
    std::string snapshot = std::move(redo_.back());
    redo_.pop_back();
    return snapshot;
}

void UndoHistory::clear() {
    undo_.clear();
    redo_.clear();
}

}  // namespace iron
