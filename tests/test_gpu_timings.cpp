#include "render/GpuTimings.h"

#include "test_framework.h"

#include <cstring>
#include <vector>

using iron::GpuScopeTable;
using iron::Mat4;
using iron::MotionHistory;

static void scopes_take_query_pairs_in_order() {
    GpuScopeTable table(8);
    CHECK(table.begin("shadows") == 0);
    CHECK(table.end() == 1);
    CHECK(table.begin("opaque") == 2);
    CHECK(table.end() == 3);
    CHECK(table.queryCount() == 4);
    CHECK(table.balanced());
}

static void nested_scopes_close_innermost_first() {
    GpuScopeTable table(8);
    CHECK(table.begin("frame") == 0);
    CHECK(table.begin("bloom") == 2);
    CHECK(!table.balanced());
    CHECK(table.end() == 3);  // bloom
    CHECK(table.end() == 1);  // frame
    CHECK(table.balanced());

    const std::vector<std::uint64_t> ticks = {100, 900, 300, 500};
    const auto timings = table.resolve(ticks, 1000.0f, 64);  // 1 tick = 1 microsecond
    CHECK(timings.size() == 2);
    CHECK(std::strcmp(timings[0].name, "frame") == 0);
    CHECK(timings[0].depth == 0);
    CHECK_NEAR(timings[0].milliseconds, 0.8f);
    CHECK(std::strcmp(timings[1].name, "bloom") == 0);
    CHECK(timings[1].depth == 1);
    CHECK_NEAR(timings[1].milliseconds, 0.2f);
}

static void a_full_table_refuses_and_stays_balanced() {
    GpuScopeTable table(1);
    CHECK(table.begin("a") == 0);
    CHECK(table.begin("b") == -1);  // Refused, but must still be ended.
    CHECK(table.end() == -1);       // Ends "b", writing nothing.
    CHECK(table.end() == 1);        // Ends "a".
    CHECK(table.balanced());
    CHECK(table.queryCount() == 2);
}

static void open_scopes_are_dropped() {
    GpuScopeTable table(4);
    table.begin("closed");
    table.end();
    table.begin("open");
    const std::vector<std::uint64_t> ticks = {0, 10, 20, 0};
    const auto timings = table.resolve(ticks, 1.0f, 64);
    CHECK(timings.size() == 1);
    CHECK(std::strcmp(timings[0].name, "closed") == 0);
}

static void the_counter_wraps_at_its_valid_bits() {
    GpuScopeTable table(1);
    table.begin("wrap");
    table.end();
    // A 32-bit counter that wrapped from 0xFFFFFF00 to 0x100: 0x200 ticks passed.
    const std::vector<std::uint64_t> ticks = {0xFFFFFF00ull, 0x100ull};
    const auto timings = table.resolve(ticks, 1e6f, 32);  // 1 tick = 1 ms
    CHECK(timings.size() == 1);
    CHECK_NEAR(timings[0].milliseconds, 512.0f);
}

static void clearing_starts_a_new_frame() {
    GpuScopeTable table(4);
    table.begin("x");
    table.clear();
    CHECK(table.queryCount() == 0);
    CHECK(table.balanced());
    CHECK(table.begin("y") == 0);
}

static Mat4 at(float x) {
    Mat4 m = Mat4::identity();
    m.m[12] = x;
    return m;
}

static void a_new_object_has_no_motion() {
    MotionHistory history;
    const Mat4 previous = history.previous(7, at(3.0f));
    CHECK_NEAR(previous.m[12], 3.0f);
}

static void an_object_reports_last_frames_transform() {
    MotionHistory history;
    history.previous(7, at(1.0f));
    history.endFrame();
    const Mat4 previous = history.previous(7, at(2.0f));
    CHECK_NEAR(previous.m[12], 1.0f);
    history.endFrame();
    CHECK_NEAR(history.previous(7, at(4.0f)).m[12], 2.0f);
}

static void an_object_not_drawn_is_forgotten() {
    MotionHistory history;
    history.previous(1, at(1.0f));
    history.previous(2, at(5.0f));
    history.endFrame();
    history.previous(1, at(1.5f));  // Object 2 skips a frame.
    history.endFrame();
    CHECK(history.tracked() == 1);
    CHECK_NEAR(history.previous(2, at(9.0f)).m[12], 9.0f);  // So it starts again still.
}

int main() {
    scopes_take_query_pairs_in_order();
    nested_scopes_close_innermost_first();
    a_full_table_refuses_and_stays_balanced();
    open_scopes_are_dropped();
    the_counter_wraps_at_its_valid_bits();
    clearing_starts_a_new_frame();
    a_new_object_has_no_motion();
    an_object_reports_last_frames_transform();
    an_object_not_drawn_is_forgotten();
    return iron_test_result();
}
