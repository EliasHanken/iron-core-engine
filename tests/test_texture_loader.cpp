#include "test_framework.h"
#include "render/TextureLoader.h"

#include <vector>

using namespace iron;

int main() {
    // invertRGBChannels: R, G, B are flipped to 255-x; alpha preserved.
    {
        std::vector<unsigned char> pixels = {
            0,   0,   0,   255,   // pixel 0: black -> white
            255, 255, 255, 200,   // pixel 1: white -> black, alpha preserved
            100, 50,  200, 0,     // pixel 2: arbitrary -> 155,205,55; alpha 0 stays 0
        };
        invertRGBChannels(pixels);
        CHECK(pixels[0]  == 255); CHECK(pixels[1]  == 255); CHECK(pixels[2]  == 255); CHECK(pixels[3]  == 255);
        CHECK(pixels[4]  == 0);   CHECK(pixels[5]  == 0);   CHECK(pixels[6]  == 0);   CHECK(pixels[7]  == 200);
        CHECK(pixels[8]  == 155); CHECK(pixels[9]  == 205); CHECK(pixels[10] == 55);  CHECK(pixels[11] == 0);
    }

    // Empty input is a no-op (doesn't crash).
    {
        std::vector<unsigned char> empty;
        invertRGBChannels(empty);
        CHECK(empty.empty());
    }

    // loadRoughnessAsSpec: end-to-end disk load of the CC0 wood roughness PNG.
    {
        // IRON_REPO_ROOT is defined by target_compile_definitions in CMakeLists.txt
        // so this path is always absolute regardless of build layout.
        const std::string path =
            std::string{IRON_REPO_ROOT} + "/assets/cc0/wood/roughness.png";

        int w = 0, h = 0;
        std::vector<unsigned char> pixels = loadRoughnessAsSpec(path, w, h);

        // Polyhaven 1k textures are 1024x1024.
        CHECK(!pixels.empty());
        CHECK(w == 1024);
        CHECK(h == 1024);
        CHECK(pixels.size() == static_cast<std::size_t>(w) * h * 4);

        // Non-existent file: returns empty, leaves out-params untouched.
        int badW = 7, badH = 9;
        std::vector<unsigned char> bad = loadRoughnessAsSpec("nonexistent.png", badW, badH);
        CHECK(bad.empty());
        CHECK(badW == 7);
        CHECK(badH == 9);
    }

    return iron_test_result();
}
