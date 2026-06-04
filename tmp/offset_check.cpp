#include <cstddef>
#include <cstdio>

struct OutlinePush {
    float color[4];
    float texel[2];
    float width;
    float exposure;
};

struct GlowCompositePush {
    float color[4];
    float intensity;
    float exposure;
    float _pad[2];
};

struct XRayPush {
    float color[4];
    float intensity;
    float exposure;
    float _pad[2];
};

struct OutlinePush_OLD {
    float color[4];
    float texel[2];
    float width;
    float _pad;
};

struct GlowCompositePush_OLD {
    float color[4];
    float intensity;
    float _pad[3];
};

struct XRayPush_OLD {
    float color[4];
    float intensity;
    float _pad[3];
};

int main() {
    printf("=== OutlinePush ===\n");
    printf("  sizeof = %zu\n", sizeof(OutlinePush));
    printf("  color    offset = %zu\n", offsetof(OutlinePush, color));
    printf("  texel    offset = %zu\n", offsetof(OutlinePush, texel));
    printf("  width    offset = %zu\n", offsetof(OutlinePush, width));
    printf("  exposure offset = %zu\n", offsetof(OutlinePush, exposure));
    printf("  size match OLD=%zu NEW=%zu: %s\n", sizeof(OutlinePush_OLD), sizeof(OutlinePush), sizeof(OutlinePush_OLD)==sizeof(OutlinePush)?"YES":"NO");

    printf("\n=== GlowCompositePush ===\n");
    printf("  sizeof = %zu\n", sizeof(GlowCompositePush));
    printf("  color     offset = %zu\n", offsetof(GlowCompositePush, color));
    printf("  intensity offset = %zu\n", offsetof(GlowCompositePush, intensity));
    printf("  exposure  offset = %zu\n", offsetof(GlowCompositePush, exposure));
    printf("  _pad      offset = %zu\n", offsetof(GlowCompositePush, _pad));
    printf("  size match OLD=%zu NEW=%zu: %s\n", sizeof(GlowCompositePush_OLD), sizeof(GlowCompositePush), sizeof(GlowCompositePush_OLD)==sizeof(GlowCompositePush)?"YES":"NO");

    printf("\n=== XRayPush ===\n");
    printf("  sizeof = %zu\n", sizeof(XRayPush));
    printf("  color     offset = %zu\n", offsetof(XRayPush, color));
    printf("  intensity offset = %zu\n", offsetof(XRayPush, intensity));
    printf("  exposure  offset = %zu\n", offsetof(XRayPush, exposure));
    printf("  _pad      offset = %zu\n", offsetof(XRayPush, _pad));
    printf("  size match OLD=%zu NEW=%zu: %s\n", sizeof(XRayPush_OLD), sizeof(XRayPush), sizeof(XRayPush_OLD)==sizeof(XRayPush)?"YES":"NO");

    return 0;
}
