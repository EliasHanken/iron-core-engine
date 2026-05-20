#pragma once

#include <cmath>
#include <cstdio>

// Tiny assertion harness. Each test file has its own main():
//
//   #include "test_framework.h"
//   int main() {
//       CHECK(1 + 1 == 2);
//       CHECK_NEAR(0.1f + 0.2f, 0.3f);
//       return iron_test_result();
//   }
//
// A failing CHECK prints the expression and location and bumps a counter;
// iron_test_result() returns non-zero if anything failed, which CTest reads.

inline int g_ironTestFailures = 0;

inline void iron_check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::printf("FAIL: %s  (%s:%d)\n", expr, file, line);
        ++g_ironTestFailures;
    }
}

inline void iron_check_near(float a, float b, const char* expr,
                            const char* file, int line) {
    if (std::fabs(a - b) > 1e-4f) {
        std::printf("FAIL: %s  (%g != %g)  (%s:%d)\n", expr, a, b, file, line);
        ++g_ironTestFailures;
    }
}

inline int iron_test_result() {
    if (g_ironTestFailures == 0) {
        std::printf("OK - all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_ironTestFailures);
    return 1;
}

#define CHECK(cond) iron_check((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(a, b) iron_check_near((a), (b), #a " ~= " #b, __FILE__, __LINE__)
