#include "editor/EditorState.h"
#include "test_framework.h"

int main() {
    // --- Test 1: initial mode is Edit ---
    {
        iron::EditorState s;
        CHECK(s.mode() == iron::EditorState::Mode::Edit);
        CHECK(!s.isPlaying());
    }

    // --- Test 2: setMode(Play) flips isPlaying to true ---
    {
        iron::EditorState s;
        s.setMode(iron::EditorState::Mode::Play);
        CHECK(s.mode() == iron::EditorState::Mode::Play);
        CHECK(s.isPlaying());
    }

    // --- Test 3: Edit -> Play -> Edit round trip ---
    {
        iron::EditorState s;
        s.setMode(iron::EditorState::Mode::Play);
        s.setMode(iron::EditorState::Mode::Edit);
        CHECK(s.mode() == iron::EditorState::Mode::Edit);
        CHECK(!s.isPlaying());
    }

    return iron_test_result();
}
