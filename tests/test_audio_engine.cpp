#define _CRT_SECURE_NO_WARNINGS  // for std::getenv on MSVC

#include "audio/AudioEngine.h"
#include "math/Vec.h"
#include "test_framework.h"

#include <cstdlib>
#include <string>

using namespace iron;

namespace {

// Skip device-touching tests when the harness is on a headless CI box.
// IRON_CI=1 is the convention M9 established for the renderer factory test.
bool runningHeadless() {
    const char* env = std::getenv("IRON_CI");
    return env != nullptr && env[0] == '1';
}

std::string testWav() {
    return std::string(IRON_REPO_ROOT) + "/tests/assets/sfx/test-beep.wav";
}

}  // namespace

int main() {
    // Test 1: NoCrashWithoutInit — every call must be safe on an
    // uninitialized engine.
    {
        AudioEngine a;
        CHECK(a.initialized() == false);
        CHECK(a.loadSound(testWav()) == kInvalidSound);
        a.playSoundAt(kInvalidSound, Vec3{0, 0, 0});
        a.playSoundAt(1234, Vec3{0, 0, 0});  // bogus handle, still safe
        a.setListener(Vec3{0, 0, 0}, Vec3{0, 0, -1}, Vec3{0, 1, 0});
        a.shutdown();  // idempotent before init
    }

    // Test 2: InitAndLoadWav — happy path on a box that has an audio device.
    if (!runningHeadless()) {
        AudioEngine a;
        if (a.init()) {
            CHECK(a.initialized() == true);
            const SoundHandle h = a.loadSound(testWav());
            CHECK(h != kInvalidSound);
            a.shutdown();
            CHECK(a.initialized() == false);
        }
        // If init() failed locally (no device), that's also acceptable.
    }

    // Test 3: LoadMissingFileReturnsInvalid.
    if (!runningHeadless()) {
        AudioEngine a;
        if (a.init()) {
            const SoundHandle h = a.loadSound("does/not/exist.wav");
            CHECK(h == kInvalidSound);
            a.shutdown();
        }
    }

    // Test 4: PlayInvalidHandleIsNoOp — must not crash, must not touch a real
    // source.
    if (!runningHeadless()) {
        AudioEngine a;
        if (a.init()) {
            a.playSoundAt(kInvalidSound, Vec3{0, 0, 0});
            a.playSoundAt(9999, Vec3{0, 0, 0});
            a.shutdown();
        }
    }

    // Test 5: SetListenerBeforeInit — no init, must be safe.
    {
        AudioEngine a;
        a.setListener(Vec3{1, 2, 3}, Vec3{0, 0, -1}, Vec3{0, 1, 0});
    }

    return iron_test_result();
}
