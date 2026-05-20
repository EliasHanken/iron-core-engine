#include "test_framework.h"

int main() {
    CHECK(1 + 1 == 2);
    CHECK_NEAR(0.1f + 0.2f, 0.3f);
    return iron_test_result();
}
