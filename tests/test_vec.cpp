#include "test_framework.h"
#include "math/Vec.h"

using namespace iron;

int main() {
    // Addition / subtraction / scalar multiply
    Vec3 a{1.0f, 2.0f, 3.0f};
    Vec3 b{4.0f, 5.0f, 6.0f};
    Vec3 sum = a + b;
    CHECK_NEAR(sum.x, 5.0f);
    CHECK_NEAR(sum.y, 7.0f);
    CHECK_NEAR(sum.z, 9.0f);

    Vec3 diff = b - a;
    CHECK_NEAR(diff.x, 3.0f);
    CHECK_NEAR(diff.z, 3.0f);

    Vec3 scaled = a * 2.0f;
    CHECK_NEAR(scaled.y, 4.0f);

    Vec3 neg = -a;
    CHECK_NEAR(neg.x, -1.0f);

    // Dot product: a.b = 1*4 + 2*5 + 3*6 = 32
    CHECK_NEAR(dot(a, b), 32.0f);

    // Cross product: x cross y = z
    Vec3 x{1.0f, 0.0f, 0.0f};
    Vec3 y{0.0f, 1.0f, 0.0f};
    Vec3 cz = cross(x, y);
    CHECK_NEAR(cz.x, 0.0f);
    CHECK_NEAR(cz.y, 0.0f);
    CHECK_NEAR(cz.z, 1.0f);

    // Length and normalize
    Vec3 v{3.0f, 4.0f, 0.0f};
    CHECK_NEAR(length(v), 5.0f);
    Vec3 n = normalize(v);
    CHECK_NEAR(length(n), 1.0f);
    CHECK_NEAR(n.x, 0.6f);
    CHECK_NEAR(n.y, 0.8f);

    // Vec2 / Vec4 exist and add
    Vec2 p2 = Vec2{1.0f, 1.0f} + Vec2{2.0f, 3.0f};
    CHECK_NEAR(p2.x, 3.0f);
    Vec4 p4 = Vec4{1.0f, 1.0f, 1.0f, 1.0f} + Vec4{1.0f, 2.0f, 3.0f, 4.0f};
    CHECK_NEAR(p4.w, 5.0f);

    return iron_test_result();
}
