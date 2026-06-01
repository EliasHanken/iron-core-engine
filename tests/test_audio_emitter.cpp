#include "audio/AudioEmitter.h"
#include "reflection/Reflection.h"
#include "reflection/RegisterCoreTypes.h"
#include "scene/ReflectionIO.h"
#include "test_framework.h"

int main() {
    // --- Defaults ---
    {
        iron::AudioEmitter a;
        CHECK(a.wavPath.empty());
        CHECK_NEAR(a.gain, 1.0f);
        CHECK(a.loop == true);
        CHECK(a.spatial == true);
        CHECK(a.playOnStart == true);
    }

    // --- ReflectionIO round-trip preserves every field (incl. bools) ---
    {
        iron::Reflection r;
        iron::registerAudioEmitter(r);

        iron::AudioEmitter src;
        src.wavPath     = "assets/cc0/sfx/hum.wav";
        src.gain        = 0.4f;
        src.loop        = false;
        src.spatial     = false;
        src.playOnStart = false;

        const nlohmann::json j = iron::componentToJson(r, src);
        iron::AudioEmitter dst;
        iron::componentFromJson(r, dst, j);

        CHECK(dst.wavPath == "assets/cc0/sfx/hum.wav");
        CHECK_NEAR(dst.gain, 0.4f);
        CHECK(dst.loop == false);
        CHECK(dst.spatial == false);
        CHECK(dst.playOnStart == false);
    }

    return iron_test_result();
}
