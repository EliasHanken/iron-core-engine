#include "render/PostEffect.h"
#include "render/PostChainPlan.h"

#include <cstdio>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void test_effect_table_defaults() {
    iron::EffectTable t;
    CHECK(t.style(0).kind == iron::EffectKind::None);
    CHECK(t.activeKinds().empty());
}
static void test_effect_table_set_get() {
    iron::EffectTable t;
    iron::EffectStyle s; s.kind = iron::EffectKind::Outline; s.color = iron::Vec3{1,0,0}; s.width = 3.0f;
    t.setStyle(1, s);
    CHECK(t.style(1).kind == iron::EffectKind::Outline);
    CHECK(t.style(1).width == 3.0f);
}
static void test_effect_table_id0_stays_none() {
    iron::EffectTable t;
    iron::EffectStyle s; s.kind = iron::EffectKind::Outline;
    t.setStyle(0, s);
    CHECK(t.style(0).kind == iron::EffectKind::None);
}
static void test_effect_table_active_kinds() {
    iron::EffectTable t;
    iron::EffectStyle a; a.kind = iron::EffectKind::Outline;
    iron::EffectStyle b; b.kind = iron::EffectKind::XRay;
    t.setStyle(1, a); t.setStyle(2, b);
    auto ks = t.activeKinds();
    CHECK(ks.size() == 2);
    bool hasOutline = false, hasXray = false;
    for (auto k : ks) { if (k == iron::EffectKind::Outline) hasOutline = true;
                        if (k == iron::EffectKind::XRay) hasXray = true; }
    CHECK(hasOutline && hasXray);
}
static void test_effect_table_dedup_same_kind() {
    iron::EffectTable t;
    iron::EffectStyle a; a.kind = iron::EffectKind::Outline;
    t.setStyle(1, a); t.setStyle(2, a);
    CHECK(t.activeKinds().size() == 1);
}
static void test_effect_table_setting_none_clears() {
    iron::EffectTable t;
    iron::EffectStyle a; a.kind = iron::EffectKind::Outline;
    t.setStyle(1, a);
    iron::EffectStyle none; none.kind = iron::EffectKind::None;
    t.setStyle(1, none);
    CHECK(t.activeKinds().empty());
}
static void test_plan_empty_is_just_copy() {
    auto p = iron::planPostChain({});
    CHECK(p.size() == 1); CHECK(p[0] == iron::PostPass::Copy);
}
static void test_plan_outline() {
    auto p = iron::planPostChain({iron::EffectKind::Outline});
    CHECK(p.size() == 2);
    CHECK(p[0] == iron::PostPass::Copy);
    CHECK(p[1] == iron::PostPass::Outline);
}
static void test_plan_glow_is_multipass() {
    auto p = iron::planPostChain({iron::EffectKind::GlowOutline});
    CHECK(p.size() == 4);
    CHECK(p[0] == iron::PostPass::Copy);
    CHECK(p[1] == iron::PostPass::GlowBlurH);
    CHECK(p[2] == iron::PostPass::GlowBlurV);
    CHECK(p[3] == iron::PostPass::GlowComposite);
}
static void test_plan_xray() {
    auto p = iron::planPostChain({iron::EffectKind::XRay});
    CHECK(p.size() == 2);
    CHECK(p[0] == iron::PostPass::Copy);
    CHECK(p[1] == iron::PostPass::XRay);
}
static void test_plan_layer_order() {
    auto p = iron::planPostChain({iron::EffectKind::Outline, iron::EffectKind::XRay, iron::EffectKind::GlowOutline});
    CHECK(p.size() == 6);
    CHECK(p[0] == iron::PostPass::Copy);
    CHECK(p[1] == iron::PostPass::XRay);
    CHECK(p[2] == iron::PostPass::Outline);
    CHECK(p[3] == iron::PostPass::GlowBlurH);
}
static void test_pingpong_alternates() {
    CHECK(iron::pingPongSource(0) == 0);
    CHECK(iron::pingPongSource(1) == 1);
    CHECK(iron::pingPongSource(2) == 0);
    CHECK(iron::pingPongDest(0) == 1);
    CHECK(iron::pingPongDest(1) == 0);
}

int main() {
    test_effect_table_defaults();
    test_effect_table_set_get();
    test_effect_table_id0_stays_none();
    test_effect_table_active_kinds();
    test_effect_table_dedup_same_kind();
    test_effect_table_setting_none_clears();
    test_plan_empty_is_just_copy();
    test_plan_outline();
    test_plan_glow_is_multipass();
    test_plan_xray();
    test_plan_layer_order();
    test_pingpong_alternates();
    if (g_failures == 0) std::printf("All post-process tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
