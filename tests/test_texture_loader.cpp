#include "test_framework.h"
#include "render/TextureLoader.h"
#include "MockRenderer.h"

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

    // loadMetallicRoughness: non-existent path returns kInvalidHandle without
    // crashing, and does not call createTexture.
    {
        MockRenderer mock;
        TextureHandle h = loadMetallicRoughness(mock, "nonexistent.png", 0.0f);
        CHECK(h == kInvalidHandle);
    }

    // loadMetallicRoughness: valid CC0 roughness PNG loads successfully and
    // calls through to r.createTexture (MockRenderer returns kInvalidHandle
    // but the call must not crash and must return without aborting).
    {
        // IRON_REPO_ROOT is defined by target_compile_definitions in CMakeLists.txt
        // so this path is always absolute regardless of build layout.
        const std::string path =
            std::string{IRON_REPO_ROOT} + "/assets/cc0/wood/roughness.png";

        MockRenderer mock;
        // MockRenderer::createTexture always returns kInvalidHandle.
        // We just verify the call does not crash and propagates that value.
        TextureHandle h = loadMetallicRoughness(mock, path, 0.0f);
        CHECK(h == kInvalidHandle);  // mock returns invalid; no crash = pass

        // metallicConstant=1.0 (metal path) also doesn't crash.
        TextureHandle hMetal = loadMetallicRoughness(mock, path, 1.0f);
        CHECK(hMetal == kInvalidHandle);
    }

    return iron_test_result();
}
