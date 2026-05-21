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

    // A direction (w = 0) is not translated, only points (w = 1) are.
    Vec4 dir = m * Vec4{1.0f, 0.0f, 0.0f, 0.0f};
    CHECK_NEAR(dir.x, 1.0f);  // translation suppressed by w = 0
    CHECK_NEAR(dir.w, 0.0f);

    // Identity is the multiplicative identity (check every diagonal + an
    // off-diagonal from a different row, so a partial-correctness bug shows).
    Mat4 idmul = id * id;
    CHECK_NEAR(idmul.at(0, 0), 1.0f);
    CHECK_NEAR(idmul.at(1, 1), 1.0f);
    CHECK_NEAR(idmul.at(2, 2), 1.0f);
    CHECK_NEAR(idmul.at(3, 3), 1.0f);
    CHECK_NEAR(idmul.at(0, 1), 0.0f);
    CHECK_NEAR(idmul.at(1, 0), 0.0f);

    // Multiplying two translation matrices adds their translations.
    Mat4 ta = Mat4::identity();
    ta.at(0, 3) = 2.0f;
    Mat4 tb = Mat4::identity();
    tb.at(0, 3) = 3.0f;
    Mat4 tc = ta * tb;
    CHECK_NEAR(tc.at(0, 3), 5.0f);

    // Matrix multiply is NOT commutative: scale-then-translate differs from
    // translate-then-scale. Pure translations commute, so this needs a scale.
    Mat4 scale = Mat4::identity();
    scale.at(0, 0) = 2.0f;  // scale x by 2
    Mat4 trans = Mat4::identity();
    trans.at(0, 3) = 3.0f;  // translate x by 3
    Vec4 p{1.0f, 0.0f, 0.0f, 1.0f};
    Vec4 scaleThenTranslate = (trans * scale) * p;  // (1 * 2) + 3 = 5
    Vec4 translateThenScale = (scale * trans) * p;  // (1 + 3) * 2 = 8
    CHECK_NEAR(scaleThenTranslate.x, 5.0f);
    CHECK_NEAR(translateThenScale.x, 8.0f);

    return iron_test_result();
}
