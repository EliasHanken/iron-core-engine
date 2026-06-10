#include "world/Transform.h"
#include "math/Transform.h"
#include "test_framework.h"

static void test_transform_matrix_equals_inline_composition() {
    iron::Transform t;
    t.position = {1.0f, 2.0f, 3.0f};
    t.rotation = iron::Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, 0.7f);
    t.scale    = {2.0f, 0.5f, 1.5f};

    const iron::Mat4 expected = iron::translation(t.position)
                              * t.rotation.toMat4()
                              * iron::scaling(t.scale);
    for (int i = 0; i < 16; ++i)
        CHECK_NEAR(t.matrix().m[i], expected.m[i]);
}

int main() {
    test_transform_matrix_equals_inline_composition();
    return iron_test_result();
}
