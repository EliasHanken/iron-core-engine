#include "ai/BehaviorTree.h"
#include "ai/Perception.h"

#include "test_framework.h"

#include <string>

using namespace iron;

namespace {

// A task that counts its starts, ticks and aborts, and runs until told otherwise.
struct Probe {
    int        starts = 0, ticks = 0, aborts = 0;
    BtStatus   result = BtStatus::Running;
    BtPtr node(const char* name) {
        return bt::action(
            name, [this](BtContext&) { ++ticks; return result; }, [this](BtContext&) { ++starts; },
            [this](BtContext&) { ++aborts; });
    }
};

std::string path(const BtNode& n) {
    std::string out;
    n.runningPath(out);
    return out;
}

}  // namespace

static void sequences_and_selectors() {
    Blackboard board;
    BtContext ctx{board, 0.1f};
    Probe a, b;
    a.result = BtStatus::Success;
    auto seq = bt::sequence("Seq", a.node("A"), b.node("B"));
    CHECK(seq->tick(ctx) == BtStatus::Running);  // A done at once, B runs.
    CHECK(a.ticks == 1 && b.ticks == 1);
    CHECK(path(*seq) == "Seq > B");
    CHECK(seq->tick(ctx) == BtStatus::Running);
    CHECK(a.ticks == 1 && b.ticks == 2);  // Resumes at B.
    b.result = BtStatus::Failure;
    CHECK(seq->tick(ctx) == BtStatus::Failure);

    Probe c, d;
    c.result = BtStatus::Failure;
    d.result = BtStatus::Success;
    auto sel = bt::selector("Sel", c.node("C"), d.node("D"));
    CHECK(sel->tick(ctx) == BtStatus::Success);
    CHECK(c.ticks == 1 && d.ticks == 1);
}

static void waits_take_their_time() {
    Blackboard board;
    board.set("pause", 0.5f);
    BtContext ctx{board, 0.2f};
    auto w = bt::wait("W", 9.0f, "pause");
    CHECK(w->tick(ctx) == BtStatus::Running);
    CHECK(w->tick(ctx) == BtStatus::Running);
    CHECK(w->tick(ctx) == BtStatus::Success);  // 0.6 s >= 0.5 s.
}

static void conditions_abort_lower_priority_and_self() {
    Blackboard board;
    BtContext ctx{board, 0.1f};
    Probe fight, patrol;
    auto root = bt::selector("Root",
                             bt::when("Alerted", [](const Blackboard& b) { return b.get<bool>("alerted"); },
                                      BtAborts::Both, fight.node("Fight")),
                             patrol.node("Patrol"));
    CHECK(root->tick(ctx) == BtStatus::Running);
    CHECK(path(*root) == "Root > Patrol");
    CHECK(fight.ticks == 0);

    // Alerted: the patrol is cut short, the fight runs the same tick.
    board.set("alerted", true);
    root->tick(ctx);
    CHECK(patrol.aborts == 1);
    CHECK(fight.starts == 1 && fight.ticks == 1);
    CHECK(path(*root) == "Root > Alerted > Fight");

    // Calm again: the fight is cut short (Self), the patrol starts afresh.
    board.set("alerted", false);
    root->tick(ctx);
    CHECK(fight.aborts == 1);
    CHECK(patrol.starts == 2);
    CHECK(path(*root) == "Root > Patrol");
}

static void a_condition_without_self_abort_finishes_its_branch() {
    Blackboard board;
    board.set("go", true);
    BtContext ctx{board, 0.1f};
    Probe task;
    auto cond = bt::when("Go", [](const Blackboard& b) { return b.get<bool>("go"); }, BtAborts::None,
                         task.node("Task"));
    CHECK(cond->tick(ctx) == BtStatus::Running);
    board.set("go", false);
    CHECK(cond->tick(ctx) == BtStatus::Running);  // Checked only on the way in.
    CHECK(task.aborts == 0);
    task.result = BtStatus::Success;
    CHECK(cond->tick(ctx) == BtStatus::Success);
    CHECK(cond->tick(ctx) == BtStatus::Failure);  // And now it's checked again.
}

static void the_blackboard_keeps_types_apart() {
    Blackboard b;
    b.set("n", 3);
    b.set("p", Vec3{1.0f, 2.0f, 3.0f});
    CHECK(b.get<int>("n") == 3);
    CHECK(b.get<float>("n", -1.0f) == -1.0f);  // An int isn't a float.
    CHECK_NEAR(b.get<Vec3>("p").y, 2.0f);
    CHECK(!b.get<bool>("missing"));
    b.clear("n");
    CHECK(!b.has("n"));
}

// --- Perception ---

static float secondsToSpot(Perception& p, Vec3 eye, Vec3 forward, Vec3 target, float visibility) {
    float t = 0.0f;
    while (!p.state().alerted && t < 30.0f) {
        p.look(eye, forward, target, visibility, 0.05f);
        t += 0.05f;
    }
    return t;
}

static void close_in_front_is_spotted_fast_far_and_aside_slowly() {
    const Vec3 eye{0.0f, 1.6f, 0.0f};
    const Vec3 ahead{0.0f, 0.0f, -1.0f};
    Perception close, far, aside, behind, hidden;
    const float tClose = secondsToSpot(close, eye, ahead, {0.0f, 1.6f, -3.0f}, 1.0f);
    const float tFar = secondsToSpot(far, eye, ahead, {0.0f, 1.6f, -35.0f}, 1.0f);
    const float tAside = secondsToSpot(aside, eye, ahead, {-5.0f, 1.6f, -0.5f}, 1.0f);
    CHECK(tClose < 1.0f);
    CHECK(tFar > 2.0f && tFar < 30.0f);
    CHECK(tAside > tClose * 1.5f);
    CHECK(secondsToSpot(behind, eye, ahead, {0.0f, 1.6f, 5.0f}, 1.0f) >= 30.0f);
    CHECK(secondsToSpot(hidden, eye, ahead, {0.0f, 1.6f, -3.0f}, 0.0f) >= 30.0f);
}

static void a_glimpse_fades_but_an_alert_holds_then_is_forgotten() {
    const Vec3 eye{0.0f, 1.6f, 0.0f};
    const Vec3 ahead{0.0f, 0.0f, -1.0f};
    Perception p;
    // A glimpse of a far figure, then it's gone.
    for (int i = 0; i < 20; ++i) p.look(eye, ahead, {0.0f, 1.6f, -30.0f}, 1.0f, 0.05f);
    const float glimpse = p.state().level;
    CHECK(glimpse > 0.0f && !p.state().alerted);
    for (int i = 0; i < 100; ++i) p.look(eye, ahead, {0.0f, 1.6f, 30.0f}, 1.0f, 0.05f);
    CHECK(p.state().level < glimpse * 0.5f);

    // Spotted: stays alerted with the target out of sight, knows where it was.
    secondsToSpot(p, eye, ahead, {2.0f, 1.6f, -4.0f}, 1.0f);
    CHECK(p.state().alerted && p.state().hasLastKnown);
    CHECK_NEAR(p.state().lastKnown.x, 2.0f);
    for (int i = 0; i < 200; ++i) p.look(eye, ahead, {0.0f, 1.6f, 30.0f}, 1.0f, 0.05f);  // 10 s.
    CHECK(p.state().alerted && !p.state().seesTarget);
    for (int i = 0; i < 300; ++i) p.look(eye, ahead, {0.0f, 1.6f, 30.0f}, 1.0f, 0.05f);  // 25 s in all.
    CHECK(!p.state().alerted);
}

static void gunfire_alerts_footsteps_draw_a_look() {
    const Vec3 at{0.0f, 0.0f, 0.0f};
    Perception p;
    p.hear(at, {6.0f, 0.0f, 0.0f}, 4.0f, false, true);  // Out of earshot.
    CHECK(!p.consumeHeard());
    p.hear(at, {3.0f, 0.0f, 0.0f}, 4.0f, false, true);
    CHECK(p.suspicious() && !p.state().alerted);
    CHECK(p.consumeHeard());
    CHECK(!p.consumeHeard());
    p.hear(at, {30.0f, 0.0f, 10.0f}, 80.0f, true, true);
    CHECK(p.state().alerted);
    CHECK_NEAR(p.state().lastKnown.x, 30.0f);

    Perception q;
    q.share({5.0f, 0.0f, 5.0f});
    CHECK(q.state().alerted && q.state().hasLastKnown);
}

int main() {
    sequences_and_selectors();
    waits_take_their_time();
    conditions_abort_lower_priority_and_self();
    a_condition_without_self_abort_finishes_its_branch();
    the_blackboard_keeps_types_apart();
    close_in_front_is_spotted_fast_far_and_aside_slowly();
    a_glimpse_fades_but_an_alert_holds_then_is_forgotten();
    gunfire_alerts_footsteps_draw_a_look();
    return iron_test_result();
}
