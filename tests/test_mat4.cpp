#include "test_framework.h"
#include "math/Mat4.h"
#include "math/Vec.h"

using namespace iron;

int main() {
    // Identity leaves a vector unchanged.
    Mat4 id = Mat4::identity();
    Vec4 v{2.0f, 3.0f, 4.0f, 1.0f};
    Vec4 r = id * v;
    CHECK_NEAR(r.x, 2.0f);
    CHECK_NEAR(r.y, 3.0f);
    CHECK_NEAR(r.z, 4.0f);
    CHECK_NEAR(r.w, 1.0f);

    // Column-major accessor: at(row, col).
    Mat4 m = Mat4::identity();
    m.at(0, 3) = 5.0f;  // translation x in column 3
    CHECK_NEAR(m.at(0, 3), 5.0f);
    Vec4 t = m * Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    CHECK_NEAR(t.x, 6.0f);  // 1 + 5

    // Identity is the multiplicative identity.
    Mat4 idmul = id * id;
    CHECK_NEAR(idmul.at(0, 0), 1.0f);
    CHECK_NEAR(idmul.at(1, 1), 1.0f);
    CHECK_NEAR(idmul.at(0, 1), 0.0f);

    // Multiplying two translation matrices adds their translations.
    Mat4 ta = Mat4::identity();
    ta.at(0, 3) = 2.0f;
    Mat4 tb = Mat4::identity();
    tb.at(0, 3) = 3.0f;
    Mat4 tc = ta * tb;
    CHECK_NEAR(tc.at(0, 3), 5.0f);

    return iron_test_result();
}
