#include "reflection/TypeId.h"
#include "reflection/FieldDesc.h"

#include <cstdint>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void test_typeid_unknown_is_zero() {
    CHECK(static_cast<int>(iron::TypeId::Unknown) == 0);
}

static void test_fieldmeta_defaults_are_zero() {
    iron::FieldMeta m;
    CHECK(m.min == 0.0f);
    CHECK(m.max == 0.0f);
}

static void test_fielddesc_ptr_arithmetic() {
    // Simulate a struct with two fields and verify ptr<T> computes the right
    // address via the stored byte offset.
    struct Probe { int a; float b; };
    Probe p{};
    iron::FieldDesc fa{ "a", iron::TypeId::Int32,
                        static_cast<uint32_t>(offsetof(Probe, a)), {} };
    iron::FieldDesc fb{ "b", iron::TypeId::Float,
                        static_cast<uint32_t>(offsetof(Probe, b)), {} };
    CHECK(fa.ptr<int>(&p) == &p.a);
    CHECK(fb.ptr<float>(&p) == &p.b);
    *fa.ptr<int>(&p)   = 42;
    *fb.ptr<float>(&p) = 1.5f;
    CHECK(p.a == 42);
    CHECK(p.b == 1.5f);
}

int main() {
    test_typeid_unknown_is_zero();
    test_fieldmeta_defaults_are_zero();
    test_fielddesc_ptr_arithmetic();
    if (g_failures == 0) std::printf("All type-reflection tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
