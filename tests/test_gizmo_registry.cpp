// Unit tests for iron::GizmoRegistry. Uses MockRenderer to capture lines
// emitted by tick().

#include "test_framework.h"
#include "debug/GizmoRegistry.h"
#include "MockRenderer.h"

using namespace iron;

int main() {
    // addLine returns nonzero ids and they are distinct.
    {
        GizmoRegistry g;
        const GizmoId a = g.addLine("test", {0,0,0}, {1,0,0}, {1,1,1});
        const GizmoId b = g.addLine("test", {0,0,0}, {0,1,0}, {1,1,1});
        CHECK(a != kInvalidGizmo);
        CHECK(b != kInvalidGizmo);
        CHECK(a != b);
    }

    // updateLine mutates the entry's geometry/color, and lines emitted
    // after update reflect the new state.
    {
        GizmoRegistry g;
        const GizmoId id = g.addLine("test", {0,0,0}, {1,0,0}, {1,0,0});
        g.updateLine(id, {2,2,2}, {3,3,3}, {0,1,0});
        MockRenderer r;
        g.tick(0.016f, r);
        CHECK(r.lines.size() == 1);
        CHECK_NEAR(r.lines[0].a.x, 2.0f);
        CHECK_NEAR(r.lines[0].b.x, 3.0f);
        CHECK_NEAR(r.lines[0].color.y, 1.0f);
    }

    // remove() deletes a single entry; clearCategory() deletes everything
    // in that category; clearAll() empties storage.
    {
        GizmoRegistry g;
        const GizmoId a = g.addLine("cat1", {0,0,0}, {1,0,0}, {1,1,1});
        g.addLine("cat1", {0,0,0}, {2,0,0}, {1,1,1});
        g.addLine("cat2", {0,0,0}, {3,0,0}, {1,1,1});

        g.remove(a);
        MockRenderer r1;
        g.tick(0.016f, r1);
        CHECK(r1.lines.size() == 2);

        g.clearCategory("cat1");
        MockRenderer r2;
        g.tick(0.016f, r2);
        CHECK(r2.lines.size() == 1);

        g.clearAll();
        MockRenderer r3;
        g.tick(0.016f, r3);
        CHECK(r3.lines.size() == 0);
    }

    return iron_test_result();
}
