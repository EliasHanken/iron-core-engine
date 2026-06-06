#include "editor/UndoHistory.h"
#include "test_framework.h"

using namespace iron;

int main() {
    // commit -> undo returns the committed (before) snapshot; redo returns the
    // current state captured at undo time.
    {
        UndoHistory h;
        CHECK(!h.canUndo());
        CHECK(!h.canRedo());
        h.commit("A");                 // before-edit state was "A"; doc is now "B"
        CHECK(h.canUndo());
        auto u = h.undo("B");          // step back from "B" to "A"
        CHECK(u.has_value());
        CHECK(*u == "A");
        CHECK(h.canRedo());
        auto r = h.redo("A");          // step forward from "A" back to "B"
        CHECK(r.has_value());
        CHECK(*r == "B");
        CHECK(!h.canRedo());
    }

    // A fresh commit clears the redo stack.
    {
        UndoHistory h;
        h.commit("A");
        (void)h.undo("B");             // now redo has "B"
        CHECK(h.canRedo());
        h.commit("C");                 // new edit -> redo cleared
        CHECK(!h.canRedo());
    }

    // Empty-stack undo/redo are no-ops.
    {
        UndoHistory h;
        CHECK(!h.undo("X").has_value());
        CHECK(!h.redo("X").has_value());
    }

    // Capacity eviction: oldest entry is dropped; can still undo, but not past
    // the cap.
    {
        UndoHistory h(2);
        h.commit("s0");
        h.commit("s1");
        h.commit("s2");                // "s0" evicted; stack holds [s1, s2]
        auto a = h.undo("cur");
        CHECK(*a == "s2");
        auto b = h.undo("s2");
        CHECK(*b == "s1");
        CHECK(!h.canUndo());           // only 2 retained
    }

    // Round-trip invariant: undo then redo returns the same current state.
    {
        UndoHistory h;
        h.commit("before");
        auto u = h.undo("after");
        CHECK(*u == "before");
        auto r = h.redo("before");
        CHECK(*r == "after");
    }

    return 0;
}
