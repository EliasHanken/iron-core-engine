#pragma once

// The enemy: a squad of soldiers who patrol the steelworks, notice the player
// (slowly at a distance, fast up close; never through walls or smoke), go and
// look at what they heard or glimpsed, and fight as infantry do: call it out,
// break for cover, hold it (duck, then come up to aim, or step out round a tall
// one), move up or flank when the player's gone quiet, and hunt for him where
// he was last seen before they give up and go back to their rounds.
//
// Each soldier thinks with a behaviour tree (engine/ai/BehaviorTree.h) over a
// blackboard fed by his perception (engine/ai/Perception.h); the crowd on the
// navmesh moves him; Soldier animates him. The squad shares what they know:
// a shout reaches those near, the radio the rest a few seconds later.
//
//   Root
//   ├─ Alerted?         Combat: React, Hunt (lost him), Flank, Fight (cover), Stand
//   ├─ Suspicious?      Investigate: turn, approach, search, settle
//   ├─ Guard?           Watch (a post of one point)
//   └─ Patrol           walk the route, pause at its corners

#include "Smoke.h"
#include "Soldier.h"
#include "ai/BehaviorTree.h"
#include "ai/NavMesh.h"
#include "ai/Perception.h"
#include "core/Log.h"
#include "math/Vec.h"
#include "render/Renderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace foundry {

// Somewhere to take cover: a spot on the navmesh beside something solid.
struct CoverPoint {
    Vec3  at;            // Where to stand.
    Vec3  out;           // Level, unit: away from what covers it.
    Vec3  along;         // Level, unit: along its face (to step out round tall cover).
    float height = 0.0f; // Of the cover above the ground there.
    int   island = -1;   // The navmesh island it's on.
    int   takenBy = -1;  // The soldier holding it.
    bool  low() const { return height < 1.45f; }
};

// Cover beside every solid box at least waist high, every 1.2 m along each face.
inline std::vector<CoverPoint> findCover(std::span<const SmokeCollider> boxes, const iron::NavMesh& nav) {
    std::vector<CoverPoint> out;
    for (const SmokeCollider& b : boxes) {
        if (b.body.value != iron::kInvalidBody.value || b.half.y * 2.0f < 0.95f) continue;
        const float c = std::cos(b.yaw), s = std::sin(b.yaw);
        const Vec3 ax{c, 0.0f, -s}, az{s, 0.0f, c};
        const struct Face {
            Vec3  normal, tangent;
            float depth, width;
        } faces[] = {{ax, az, b.half.x, b.half.z},
                     {ax * -1.0f, az, b.half.x, b.half.z},
                     {az, ax, b.half.z, b.half.x},
                     {az * -1.0f, ax, b.half.z, b.half.x}};
        const float bottom = b.centre.y - b.half.y;
        const float top = b.centre.y + b.half.y;
        for (const Face& f : faces) {
            const float reach = std::max(f.width - 0.4f, 0.0f);
            const int count = std::max(1, static_cast<int>(reach * 2.0f / 1.2f) + 1);
            for (int i = 0; i < count; ++i) {
                const float along = count == 1 ? 0.0f : -reach + 2.0f * reach * i / (count - 1);
                Vec3 p = b.centre + f.normal * (f.depth + 0.5f) + f.tangent * along;
                p.y = bottom;
                Vec3 on;
                if (!nav.nearest(p, on, {0.25f, 0.6f, 0.25f})) continue;
                const float dx = on.x - p.x, dz = on.z - p.z;
                if (dx * dx + dz * dz > 0.04f || top - on.y < 0.95f) continue;
                const bool near = std::any_of(out.begin(), out.end(), [&](const CoverPoint& q) {
                    const Vec3 d = q.at - on;
                    return d.x * d.x + d.y * d.y + d.z * d.z < 0.5f;
                });
                if (!near) out.push_back({on, f.normal, f.tangent, std::min(top - on.y, 2.2f), nav.island(on)});
            }
        }
    }
    return out;
}

// What the game tells the squad each frame.
struct Senses {
    Vec3 playerEye{};
    Vec3 playerVelocity{};
    // How much of a sight line gets through walls and smoke (0..1), from, to.
    std::function<float(Vec3, Vec3)> visibility;
};

class Squad;

class Enemy {
public:
    Enemy(const SoldierLook& look, std::string name, std::vector<Vec3> route, float pace, float postYaw)
        : name(std::move(name)), route(std::move(route)), pace(pace), postYaw(postYaw), soldier(look) {
        at = this->route.front();
        yaw = postYaw;
        if (this->route.size() > 1) {
            const Vec3 d = this->route[1] - at;
            yaw = std::atan2(-d.x, -d.z);
        }
        headYaw = yaw;
    }

    std::string        name;
    std::string        group;    // Which lot he's with ("yard", "hall"...), for the story.
    std::vector<Vec3>  route;    // Walked round in order; one point is a post.
    float              pace;     // Patrol walking speed, m/s.
    float              postYaw;  // A guard's way of facing.
    Soldier            soldier;
    iron::Perception   perception;
    iron::Blackboard   board;
    iron::BtPtr        tree;
    std::string        doing;    // The running path down the tree.
    int                agent = -1;
    std::size_t        next = 1;

    // Where he is.
    Vec3  at{};
    float yaw = 0.0f;
    float headYaw = 0.0f;  // Where he's looking (body and spine).
    float speed = 0.0f;
    float turnSteps = 0.0f;  // A walk's steps while turning on the spot.
    float aim = 0.0f, crouch = 0.0f, lookAround = 0.0f;
    Vec2  aimOffset{0.0f, 0.0f};

    // What the running task wants of him.
    float movePace = 1.0f;     // While moving.
    float wantAim = 0.0f, wantCrouch = 0.0f, wantLook = 0.0f;
    bool  facing = false;      // Turned toward `faceAt` when he's not walking.
    Vec3  faceAt{};
    bool  aiming = false;      // Spine aimed at `faceAt` too.
    int   cover = -1;
    bool  flanker = false;
    bool  flanked = false;
    bool  reacted = false;
    bool  wasAlerted = false;
    // Fighting.
    float health = 100.0f;
    bool  dead = false;
    bool  mayFire = false;   // The running task has him up and aiming.
    int   burstLeft = 0;
    float shotIn = 0.0f;
    float focus = 0.0f;      // Zeroing in: 0 just got sight of the player, 1 settled on him.
    std::vector<std::pair<int, float>> shunned;  // Cover that failed him, and when.

    Vec3 eye() const { return at + Vec3{0.0f, crouch > 0.5f ? 1.05f : 1.6f, 0.0f}; }
    bool guard() const { return route.size() == 1; }
};

class Squad {
public:
    Squad(iron::NavMesh& nav, std::vector<CoverPoint> cover) : nav_(nav), cover_(std::move(cover)) {}

    void add(const SoldierLook& look, std::string name, std::vector<Vec3> route, float pace, float postYaw = 0.0f,
             std::string group = "yard") {
        auto e = std::make_unique<Enemy>(look, std::move(name), std::move(route), pace, postYaw);
        e->group = std::move(group);
        // A route that crosses between islands (into a shut room, onto a container) can't be walked.
        for (const Vec3& p : e->route) {
            if (nav_.island(p) != nav_.island(e->route.front())) {
                iron::Log::warn("foundry: %s's route leaves its navmesh island at (%.1f %.1f)", e->name.c_str(), p.x, p.z);
            }
        }
        e->agent = nav_.addAgent(e->at, 0.35f, std::max(pace, 0.5f));
        e->tree = think(*e);
        enemies_.push_back(std::move(e));
    }

    // A sound: heard by everyone within `range`.
    void noise(Vec3 at, float range, bool alarming, bool fromPlayer) {
        for (auto& e : enemies_) {
            if (!e->dead) e->perception.hear(e->eye(), at, range, alarming, fromPlayer);
        }
    }

    // --- Fighting ---
    // A round each soldier fired this frame, for the game to fly.
    struct Shot {
        Vec3 from;
        Vec3 direction;
        int  shooter;
    };
    const std::vector<Shot>& shots() const { return shots_; }
    // Where bodies hit the ground this frame.
    const std::vector<Vec3>& falls() const { return landed_; }

    // The soldier, and the part of him, a round along this ray meets first.
    struct Target {
        int              enemy = -1;
        iron::RagdollHit hit;
    };
    Target shoot(Vec3 from, Vec3 direction, float maxDistance) const {
        Target best;
        best.hit.distance = maxDistance;
        for (std::size_t i = 0; i < enemies_.size(); ++i) {
            const Enemy& e = *enemies_[i];
            if (e.dead) continue;
            // Nowhere near the line: not worth the parts.
            const Vec3 c = e.at + Vec3{0.0f, 1.0f, 0.0f} - from;
            const float along = iron::dot(c, direction);
            if (along < -1.5f || along > best.hit.distance + 1.5f || iron::length(c - direction * along) > 1.6f) continue;
            const iron::RagdollHit h = e.soldier.hit(from, direction, best.hit.distance);
            if (h.part >= 0 && h.distance < best.hit.distance) {
                best.enemy = static_cast<int>(i);
                best.hit = h;
            }
        }
        return best;
    }
    const iron::RagdollPartSpec& part(int enemy, int part) const {
        return enemies_[static_cast<std::size_t>(enemy)]->soldier.part(part);
    }

    // A round (or a blast) into soldier `enemy`'s `part`: `amount` before the part's
    // own multiplier, travelling `direction`, from `shooter`. True if it killed him.
    bool damage(int enemy, int part, float amount, Vec3 direction, Vec3 at, Vec3 shooter, iron::PhysicsWorld& world,
                float shove = 60.0f) {
        Enemy& e = *enemies_[static_cast<std::size_t>(enemy)];
        if (e.dead) {
            e.soldier.push(part, direction * shove * 0.5f, at);
            return false;
        }
        const iron::RagdollPartSpec& spec = e.soldier.part(part);
        if (spec.damage <= 0.0f) return false;  // Into his rifle.
        e.health -= amount * spec.damage;
        // He knows where that came from.
        e.perception.hear(e.eye(), shooter, 1.0e9f, true, true);
        if (e.health > 0.0f) {
            const Vec3 side{std::cos(e.yaw), 0.0f, -std::sin(e.yaw)};
            e.soldier.flinch({-0.35f * iron::dot(direction, side), 0.25f});
            return false;
        }
        e.dead = true;
        e.health = 0.0f;
        const Vec3 velocity = e.agent >= 0 ? nav_.agentVelocity(e.agent) : Vec3{};
        release(e);
        if (e.agent >= 0) nav_.removeAgent(e.agent);
        e.agent = -1;
        e.soldier.die(world, velocity, part, direction * shove * (spec.damage >= 4.0f ? 1.4f : 1.0f), at,
                      1000u + static_cast<std::uint32_t>(enemy));
        falling_.push_back({e.soldier.centre(), 0.55f + 0.2f * random()});
        // Those near see or hear him go down.
        for (auto& o : enemies_) {
            if (!o->dead && o.get() != &e) o->perception.hear(o->eye(), e.at, 25.0f, true, false);
        }
        return true;
    }

    // A grenade: everyone near is hurt by how close, and thrown.
    void blast(Vec3 at, float radius, iron::PhysicsWorld& world) {
        for (std::size_t i = 0; i < enemies_.size(); ++i) {
            Enemy& e = *enemies_[i];
            const Vec3 c = e.soldier.centre();
            const float d = iron::length(c - at);
            if (d > radius) continue;
            const float f = 1.0f - d / radius;
            const Vec3 away = iron::normalize(c - at + Vec3{0.0f, 0.6f, 0.0f});
            damage(static_cast<int>(i), 1, 220.0f * f * f, away, c, at, world, 260.0f * f);
        }
    }

    // The player's down: they stand down and go back to their rounds.
    void playerDown() {
        for (auto& e : enemies_) {
            if (e->dead) continue;
            e->perception.calmDown();
            e->board.clear("look_into");
            e->burstLeft = 0;
        }
        sinceSeen_ = 1.0e9f;
        radioIn_ = -1.0f;
    }
    // An alarm: everyone knows there's someone at `at`.
    void alertAll(Vec3 at) {
        for (auto& e : enemies_) {
            if (!e->dead) e->perception.share(at);
        }
    }
    // Brings in a soldier who already knows where the player was.
    void addAlerted(const SoldierLook& look, std::string name, Vec3 at, std::string group, Vec3 threat) {
        add(look, std::move(name), {at}, 0.0f, 0.0f, std::move(group));
        enemies_.back()->perception.share(threat);
    }
    int alive(const std::string& group) const {
        return static_cast<int>(std::count_if(enemies_.begin(), enemies_.end(),
                                              [&](const auto& e) { return !e->dead && e->group == group; }));
    }
    int alive() const {
        return static_cast<int>(std::count_if(enemies_.begin(), enemies_.end(), [](const auto& e) { return !e->dead; }));
    }

    void update(float dt, const Senses& senses) {
        senses_ = &senses;
        shots_.clear();
        landed_.clear();
        if (dt <= 0.0f) return;
        clock_ += dt;
        for (auto& f : falling_) f.second -= dt;
        for (const auto& f : falling_) {
            if (f.second <= 0.0f) landed_.push_back(f.first);
        }
        std::erase_if(falling_, [](const auto& f) { return f.second <= 0.0f; });
        // --- Looking ---
        bool anyoneSees = false;
        for (auto& e : enemies_) {
            if (e->dead) continue;
            const Vec3 forward{-std::sin(e->headYaw), 0.0f, -std::cos(e->headYaw)};
            const Vec3 eye = e->eye();
            const Vec3 to = senses.playerEye - eye;
            const float vis = iron::length(to) < e->perception.settings().range ? senses.visibility(eye, senses.playerEye)
                                                                                : 0.0f;
            // Still, he's harder to pick out than on the move.
            const Vec3 v = senses.playerVelocity;
            const float moving = std::min(std::sqrt(v.x * v.x + v.z * v.z) / 4.0f, 1.0f);
            e->perception.look(eye, forward, senses.playerEye, vis * (0.55f + 0.45f * moving), dt);
            if (e->perception.state().seesTarget && e->perception.state().alerted) {
                anyoneSees = true;
                known_ = e->perception.state().lastKnown;
            }
        }
        sinceSeen_ = anyoneSees ? 0.0f : sinceSeen_ + dt;
        // --- Telling each other ---
        for (auto& e : enemies_) {
            if (e->dead) continue;
            const bool alerted = e->perception.state().alerted;
            if (alerted && !e->wasAlerted) {
                // A shout to those near; the radio to everyone else shortly.
                const Vec3 where = e->perception.state().lastKnown;
                for (auto& o : enemies_) {
                    if (!o->dead && o.get() != e.get() && iron::length(o->at - e->at) < 30.0f) o->perception.share(where);
                }
                if (radioIn_ < 0.0f) {
                    radioIn_ = 3.0f;
                    radioAt_ = where;
                }
            }
            e->wasAlerted = alerted;
        }
        if (radioIn_ >= 0.0f && (radioIn_ -= dt) < 0.0f) {
            for (auto& o : enemies_) {
                if (!o->dead) o->perception.share(radioAt_);
            }
        }
        // Who can see him keeps the others posted.
        if (anyoneSees) {
            for (auto& e : enemies_) {
                if (!e->dead && e->perception.state().alerted && !e->perception.state().seesTarget) {
                    e->perception.share(known_);
                }
            }
        }
        assignFlanker();
        // --- Thinking ---
        for (auto& e : enemies_) {
            if (e->dead) continue;
            brief(*e);
            iron::BtContext ctx{e->board, dt};
            // A tree that's finished starts over at once, not a frame later.
            if (e->tree->tick(ctx) != iron::BtStatus::Running) e->tree->tick(ctx);
            std::string was = std::move(e->doing);
            e->doing.clear();
            e->tree->runningPath(e->doing);
            if (log && e->doing != was) {
                iron::Log::info("ai %6.2f %-5s %s (crouch %.1f aim %.1f)", clock_, e->name.c_str(), e->doing.c_str(),
                                e->crouch, e->aim);
            }
        }
        // --- Moving ---
        nav_.updateCrowd(dt);
        for (auto& e : enemies_) {
            if (e->dead) {
                e->soldier.update(dt, {});
                continue;
            }
            move(*e, dt);
            fire(*e, dt);
        }
    }

    int draw(iron::Renderer& renderer, std::uint32_t motionBase) const {
        int draws = 0;
        for (std::size_t i = 0; i < enemies_.size(); ++i) {
            draws += enemies_[i]->soldier.draw(renderer, motionBase + 10u * static_cast<std::uint32_t>(i));
        }
        return draws;
    }

    // For the debug view: cover (taken in orange), what each sees and is after.
    void debugLines(iron::Renderer& renderer) const {
        const Vec3 up{0.0f, 1.0f, 0.0f};
        for (const CoverPoint& c : cover_) {
            const Vec3 colour = c.takenBy >= 0 ? Vec3{1.0f, 0.5f, 0.1f} : c.low() ? Vec3{0.3f, 0.6f, 0.3f}
                                                                                    : Vec3{0.3f, 0.45f, 0.7f};
            renderer.drawLineOverlay(c.at + up * 0.05f, c.at + up * 0.05f + c.out * 0.35f, colour);
        }
        for (const auto& e : enemies_) {
            if (e->dead) continue;
            const iron::Awareness& a = e->perception.state();
            const Vec3 head = e->at + up * 2.05f;
            const Vec3 colour = a.alerted ? Vec3{1.0f, 0.15f, 0.1f}
                                          : Vec3{0.3f + 0.7f * a.level / 0.3f, 1.0f - 0.4f * a.level, 0.2f};
            renderer.drawLineOverlay(head, head + up * (0.1f + 0.5f * a.level), colour);
            const Vec3 forward{-std::sin(e->headYaw), 0.0f, -std::cos(e->headYaw)};
            renderer.drawLineOverlay(e->eye(), e->eye() + forward * 2.0f, {0.9f, 0.9f, 0.9f});
            if (a.hasLastKnown && (a.alerted || e->perception.suspicious())) {
                // Where he thinks the player is: a cross on the ground.
                const Vec3 k = a.lastKnown + up * 0.05f;
                renderer.drawLineOverlay(k - Vec3{0.3f, 0.0f, 0.3f}, k + Vec3{0.3f, 0.0f, 0.3f}, colour);
                renderer.drawLineOverlay(k - Vec3{0.3f, 0.0f, -0.3f}, k + Vec3{0.3f, 0.0f, -0.3f}, colour);
            }
        }
    }

    struct Label {
        Vec3        at;
        std::string text;
        Vec3        colour;
        int         line;  // Of the label's lines, top first.
    };
    void labels(std::vector<Label>& out) const {
        for (const auto& e : enemies_) {
            if (e->dead) continue;
            const iron::Awareness& a = e->perception.state();
            const char* mood = a.alerted ? "ALERT" : e->perception.suspicious() ? "SUSPICIOUS" : "CALM";
            char head[64];
            std::snprintf(head, sizeof(head), "%s  %s %.0f%%", e->name.c_str(), mood, a.level * 100.0f);
            // The leaf and its parent read best: "FIGHT > HOLDCOVER".
            std::string path = e->doing;
            const auto last = path.rfind(" > ");
            if (last != std::string::npos && last > 0) {
                const auto before = path.rfind(" > ", last - 1);
                if (before != std::string::npos) path = path.substr(before + 3);
            }
            std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) { return std::toupper(ch); });
            const Vec3 colour = a.alerted ? Vec3{1.0f, 0.35f, 0.3f} : e->perception.suspicious() ? Vec3{1.0f, 0.85f, 0.3f}
                                                                                                  : Vec3{0.75f, 1.0f, 0.75f};
            out.push_back({e->at + Vec3{0.0f, 2.25f, 0.0f}, head, colour, 0});
            out.push_back({e->at + Vec3{0.0f, 2.25f, 0.0f}, path, colour, 1});
        }
    }

    const std::vector<std::unique_ptr<Enemy>>& enemies() const { return enemies_; }
    const std::vector<CoverPoint>& cover() const { return cover_; }
    bool log = false;  // Each change of what a soldier's doing, to the log.
    float accuracy = 1.0f;  // How well they shoot: the difficulty.

private:
    // --- The blackboard, from perception and the squad ---
    void brief(Enemy& e) {
        const iron::Awareness& a = e.perception.state();
        if (!a.alerted) {
            // Stood down: the next alert starts with a reaction again.
            e.reacted = false;
            release(e);
        }
        e.board.set("alerted", a.alerted);
        e.board.set("suspicious", e.perception.suspicious() && !a.alerted);
        e.board.set("since_seen", sinceSeen_);
        if (a.hasLastKnown) e.board.set("last_known", a.lastKnown);
        // Something to look into: what was heard, or glimpsed, whichever's newer.
        const bool heard = e.perception.consumeHeard();
        if (heard || !e.board.has("look_into")) {
            e.board.set("look_into", heard || !a.hasLastKnown ? a.heardAt : a.lastKnown);
        } else if (a.hasLastKnown && a.sinceSeen < a.sinceHeard) {
            e.board.set("look_into", a.lastKnown);
        }
        if (heard) e.board.set("heard_new", true);
    }

    // --- Tasks ---
    struct Move {
        bool  ok = false;
        float stuck = 0.0f;
        Vec3  to{};
    };
    // Walks (or runs) to wherever `where` says when it starts, at `pace`.
    iron::BtPtr moveTo(Enemy& e, std::string name, std::function<Vec3()> where, std::function<float()> pace, float aim,
                       float within) {
        auto state = std::make_shared<Move>();
        return iron::bt::action(
            std::move(name),
            [this, &e, state, within](iron::BtContext& ctx) {
                if (!state->ok) {
                    if (log) iron::Log::info("ai %-5s can't head for (%.1f %.1f %.1f)", e.name.c_str(), state->to.x,
                                             state->to.y, state->to.z);
                    return iron::BtStatus::Failure;
                }
                if (nav_.arrived(e.agent, within)) return iron::BtStatus::Success;
                state->stuck = e.speed < 0.05f ? state->stuck + ctx.dt : 0.0f;
                if (state->stuck > 2.5f) {
                    if (log) {
                        const Vec3 p = nav_.agentPosition(e.agent);
                        iron::Log::info("ai %-5s stuck at (%.1f %.1f %.1f) going to (%.1f %.1f %.1f)", e.name.c_str(),
                                        p.x, p.y, p.z, state->to.x, state->to.y, state->to.z);
                    }
                    if (e.cover >= 0) shun(e);
                    return iron::BtStatus::Failure;
                }
                return iron::BtStatus::Running;
            },
            [this, &e, state, where, pace, aim](iron::BtContext&) {
                state->to = where();
                state->ok = nav_.setTarget(e.agent, state->to);
                state->stuck = 0.0f;
                e.movePace = pace();
                nav_.setMaxSpeed(e.agent, e.movePace);
                e.wantAim = aim;
                e.wantCrouch = 0.0f;
                e.wantLook = 0.0f;
                e.facing = false;
                e.aiming = false;
                e.mayFire = false;
            },
            [this, &e](iron::BtContext&) { nav_.stop(e.agent); });
    }

    // Stands still for `seconds`, as `setup` sets him.
    iron::BtPtr hold(Enemy& e, std::string name, float seconds, std::function<void(Enemy&)> setup) {
        auto left = std::make_shared<float>(0.0f);
        return iron::bt::action(
            std::move(name),
            [left](iron::BtContext& ctx) {
                *left -= ctx.dt;
                return *left <= 0.0f ? iron::BtStatus::Success : iron::BtStatus::Running;
            },
            [this, &e, left, seconds, setup](iron::BtContext&) {
                *left = seconds * (0.8f + 0.4f * random());
                nav_.stop(e.agent);
                e.facing = false;
                e.aiming = false;
                e.wantLook = 0.0f;
                e.mayFire = false;
                setup(e);
            });
    }

    iron::BtPtr pickCover(Enemy& e, bool flank) {
        return iron::bt::action(flank ? "PickFlank" : "PickCover", [this, &e, flank](iron::BtContext& ctx) {
            release(e);
            const int best = bestCover(e, ctx.board.get<Vec3>("last_known", e.at), flank);
            if (best < 0) {
                if (log) iron::Log::info("ai %-5s no %s cover at (%.1f %.1f)", e.name.c_str(), flank ? "flank" : "", e.at.x, e.at.z);
                return iron::BtStatus::Failure;
            }
            cover_[best].takenBy = index(e);
            e.cover = best;
            return iron::BtStatus::Success;
        });
    }

    // In cover: down behind it, then up to aim (over low cover, or out round the
    // side of tall), and down again; after a few goes, move on. Fails when the
    // player can see him in it.
    struct Hold {
        bool  up = false;
        float left = 0.0f;
        int   rounds = 0;
        float check = 0.0f;
        float exposed = 0.0f;  // Seconds seen where he hides, lately.
        Vec3  side{};
    };
    iron::BtPtr holdCover(Enemy& e) {
        auto st = std::make_shared<Hold>();
        return iron::bt::action(
            "HoldCover",
            [this, &e, st](iron::BtContext& ctx) {
                if (e.cover < 0) return iron::BtStatus::Failure;
                const CoverPoint& c = cover_[e.cover];
                const Vec3 threat = ctx.board.get<Vec3>("last_known", e.at);
                e.faceAt = threat;
                e.facing = true;
                e.aiming = true;
                e.mayFire = st->up && e.speed < 0.4f;
                if (!st->up) {
                    e.wantCrouch = c.low() ? 1.0f : 0.0f;
                    e.wantAim = c.low() ? 1.0f : 0.45f;
                    // Seen where he hides for a while: this cover's no good.
                    if ((st->check -= ctx.dt) < 0.0f) {
                        st->check = 0.2f;
                        const bool seen = (e.crouch > 0.8f || !c.low()) &&
                                          ctx.board.get<float>("since_seen") < 1.0f &&
                                          senses_->visibility(senses_->playerEye, e.eye()) > 0.35f;
                        st->exposed = seen ? st->exposed + 0.2f : std::max(st->exposed - 0.1f, 0.0f);
                        if (st->exposed > 0.8f) {
                            if (log) iron::Log::info("ai %-5s seen in cover", e.name.c_str());
                            shun(e);
                            return iron::BtStatus::Failure;
                        }
                    }
                }
                if ((st->left -= ctx.dt) > 0.0f) return iron::BtStatus::Running;
                if (!st->up) {
                    // Up: over it, or out round whichever side of it sees the threat.
                    st->up = true;
                    st->left = 1.6f + 1.6f * random();
                    e.wantCrouch = 0.0f;
                    e.wantAim = 1.0f;
                    if (!c.low()) {
                        st->side = c.at;
                        for (const float s : {0.9f, -0.9f, 1.4f, -1.4f}) {
                            const Vec3 p = c.at + c.along * s;
                            Vec3 on;
                            if (nav_.nearest(p, on, {0.2f, 0.5f, 0.2f}) &&
                                senses_->visibility(on + Vec3{0.0f, 1.6f, 0.0f}, threat + Vec3{0.0f, 1.5f, 0.0f}) > 0.4f) {
                                st->side = on;
                                break;
                            }
                        }
                        e.movePace = 1.4f;
                        nav_.setTarget(e.agent, st->side);
                    }
                    return iron::BtStatus::Running;
                }
                // Back down.
                st->up = false;
                st->left = 1.2f + 1.4f * random();
                if (!c.low()) {
                    e.movePace = 1.4f;
                    nav_.setTarget(e.agent, c.at);
                }
                if (++st->rounds >= 3) {
                    release(e);
                    return iron::BtStatus::Success;
                }
                return iron::BtStatus::Running;
            },
            [this, &e, st](iron::BtContext&) {
                *st = Hold{};
                st->left = 1.0f + random();
                nav_.stop(e.agent);
            },
            [this, &e](iron::BtContext&) { release(e); });
    }

    // --- The tree ---
    iron::BtPtr think(Enemy& e) {
        using namespace iron;
        const auto flag = [](const char* key) { return [key](const iron::Blackboard& b) { return b.get<bool>(key); }; };
        const auto lastKnown = [&e]() { return e.board.get<Vec3>("last_known", e.at); };
        const auto lookInto = [&e]() { return e.board.get<Vec3>("look_into", e.at); };

        BtPtr combat = bt::selector(
            "Combat",
            // First: stop, turn on him, rifle up, shout.
            bt::when("React", [&e](const iron::Blackboard&) { return !e.reacted; }, BtAborts::None,
                     hold(e, "Spot", 0.7f,
                          [lastKnown](Enemy& me) {
                              me.faceAt = lastKnown();
                              me.facing = me.aiming = true;
                              me.wantAim = 1.0f;
                              me.wantCrouch = 0.0f;
                              me.reacted = true;
                          })),
            // Nobody's seen him for a while: go to where he was, and search.
            bt::when("Hunt", [](const iron::Blackboard& b) { return b.get<float>("since_seen") > 12.0f; },
                     BtAborts::Both,
                     bt::sequence("Hunt", moveTo(e, "Advance", lastKnown, [] { return 1.5f; }, 0.8f, 2.0f),
                                  hold(e, "Sweep", 5.0f,
                                       [](Enemy& me) {
                                           me.wantAim = 1.0f;
                                           me.wantLook = 0.5f;
                                           me.mayFire = true;
                                       }))),
            // The flanker swings wide while the rest hold him.
            bt::when("Flank",
                     [&e](const iron::Blackboard& b) {
                         return e.flanker && !e.flanked && b.get<float>("since_seen") > 3.0f;
                     },
                     BtAborts::LowerPriority,
                     bt::sequence("Flank", pickCover(e, true),
                                  moveTo(e, "Swing", [this, &e] { return coverAt(e); }, [] { return 2.9f; }, 0.0f, 0.25f),
                                  bt::action("Flanked", [&e](iron::BtContext&) {
                                      e.flanked = true;
                                      return iron::BtStatus::Success;
                                  }),
                                  holdCover(e))),
            bt::sequence("Fight", pickCover(e, false),
                         moveTo(e, "Break", [this, &e] { return coverAt(e); }, [] { return 2.9f; }, 0.0f, 0.25f),
                         holdCover(e)),
            // No cover to be had: stand and aim a moment, then think again.
            hold(e, "Stand", 1.5f, [lastKnown](Enemy& me) {
                me.faceAt = lastKnown();
                me.facing = me.aiming = true;
                me.wantAim = 1.0f;
                me.mayFire = true;
            }));

        BtPtr investigate = bt::sequence(
            "Investigate",
            hold(e, "Turn", 1.2f,
                 [lookInto](Enemy& me) {
                     me.faceAt = lookInto();
                     me.facing = me.aiming = true;
                     me.wantAim = 0.5f;
                 }),
            moveTo(e, "Approach", lookInto, [] { return 1.0f; }, 0.6f, 1.5f),
            hold(e, "Search", 5.0f,
                 [](Enemy& me) {
                     me.wantAim = 0.3f;
                     me.wantLook = 1.0f;
                 }),
            bt::action("Settle", [&e](iron::BtContext& ctx) {
                if (!e.perception.state().alerted) e.perception.calmDown();
                ctx.board.clear("look_into");
                return iron::BtStatus::Success;
            }));

        BtPtr patrol = bt::sequence(
            "Patrol",
            moveTo(e, "Walk", [&e] { return e.route[e.next % e.route.size()]; }, [&e] { return e.pace; }, 0.0f, 0.6f),
            hold(e, "Pause", 1.2f, [](Enemy& me) { me.wantLook = me.next % 2 == 0 ? 1.0f : 0.0f; }),
            bt::action("Next", [&e](iron::BtContext&) {
                e.next = (e.next + 1) % e.route.size();
                return iron::BtStatus::Success;
            }));

        return bt::selector(
            "Root", bt::when("Alerted", flag("alerted"), BtAborts::Both, std::move(combat)),
            bt::when("Suspicious", flag("suspicious"), BtAborts::LowerPriority, std::move(investigate)),
            bt::when("Guard", [&e](const iron::Blackboard&) { return e.guard(); }, BtAborts::None,
                     bt::sequence("Post", hold(e, "Watch", 6.0f, [](Enemy& me) {
                                      me.faceAt = me.route.front() +
                                                  Vec3{-std::sin(me.postYaw), 0.0f, -std::cos(me.postYaw)} * 5.0f;
                                      me.facing = true;
                                  }),
                                  hold(e, "LookAbout", 5.0f, [](Enemy& me) { me.wantLook = 1.0f; }))),
            std::move(patrol));
    }

    // --- Moving and animating ---
    void move(Enemy& e, float dt) {
        if (e.agent >= 0) {
            e.at = nav_.agentPosition(e.agent);
            const Vec3 v = nav_.agentVelocity(e.agent);
            const float ground = std::sqrt(v.x * v.x + v.z * v.z);
            e.speed += (ground - e.speed) * std::min(dt * 6.0f, 1.0f);
            float wantYaw = e.yaw;
            // Walking, he faces the way he goes (the clips walk forward); standing, what he's watching.
            if (ground > 0.3f) {
                wantYaw = std::atan2(-v.x, -v.z);
            } else if (e.facing) {
                const Vec3 to = e.faceAt - e.at;
                if (to.x * to.x + to.z * to.z > 0.01f) wantYaw = std::atan2(-to.x, -to.z);
            }
            const float turn = wrap(wantYaw - e.yaw);
            const float step = std::clamp(turn, -3.0f * dt, 3.0f * dt);
            e.yaw = wrap(e.yaw + step);
            // Turning on the spot, he steps round rather than spinning on his heels.
            const float turning = std::abs(step) / std::max(dt, 1e-4f);
            e.turnSteps += ((turning > 0.6f && ground < 0.3f ? 0.35f + 0.25f * std::min(turning / 3.0f, 1.0f) : 0.0f) -
                            e.turnSteps) * std::min(dt * 8.0f, 1.0f);
            // He slows while he turns into a new leg.
            if (ground > 0.1f) nav_.setMaxSpeed(e.agent, e.movePace * std::clamp(1.0f - std::abs(turn) / 1.6f, 0.2f, 1.0f));
        }
        const float ease = std::min(dt * 3.0f, 1.0f);
        e.aim += (e.wantAim - e.aim) * ease;
        e.crouch += (e.wantCrouch - e.crouch) * std::min(dt * 4.0f, 1.0f);
        e.lookAround += (e.wantLook - e.lookAround) * std::min(dt * 2.0f, 1.0f);
        Vec2 wantOffset{0.0f, 0.0f};
        if (e.aiming) {
            const Vec3 to = e.faceAt + Vec3{0.0f, 1.4f, 0.0f} - e.eye();
            wantOffset.x = std::clamp(wrap(std::atan2(-to.x, -to.z) - e.yaw), -1.2f, 1.2f);
            wantOffset.y = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));
        }
        e.aimOffset.x += (wantOffset.x - e.aimOffset.x) * ease;
        e.aimOffset.y += (wantOffset.y - e.aimOffset.y) * ease;
        e.headYaw = e.yaw + e.aimOffset.x;
        Soldier::Pose pose;
        pose.feet = e.at;
        pose.yaw = e.yaw;
        pose.speed = std::max(e.speed, e.turnSteps);
        pose.aim = e.aim;
        pose.crouch = e.crouch;
        pose.lookAround = e.lookAround;
        pose.aimOffset = e.aimOffset;
        e.soldier.update(dt, pose);
    }

    // --- Firing ---
    // Bursts of three to six at the AK's 600 a minute while he's up, aiming and can
    // see the player; the first rounds go wide and he settles in over a second or
    // two, worse the further off and the faster the player's moving.
    void fire(Enemy& e, float dt) {
        const iron::Awareness& a = e.perception.state();
        const bool can = e.mayFire && a.seesTarget && e.aim > 0.85f;
        e.focus = can ? std::min(e.focus + dt / 1.6f, 1.0f) : std::max(e.focus - dt * 0.5f, 0.0f);
        e.shotIn -= dt;
        if (!can) {
            e.burstLeft = 0;
            return;
        }
        if (e.shotIn > 0.0f) return;
        if (e.burstLeft <= 0) e.burstLeft = 3 + static_cast<int>(random() * 3.99f);
        const Vec3 from = e.soldier.muzzle();
        const Vec3 target = senses_->playerEye - Vec3{0.0f, 0.4f, 0.0f};
        const float distance = iron::length(target - from);
        const Vec3 v = senses_->playerVelocity;
        const float moving = std::min(std::sqrt(v.x * v.x + v.z * v.z) / 5.0f, 1.0f);
        const float spread =
            (0.012f + 0.0009f * distance + 0.03f * moving + 0.055f * (1.0f - e.focus)) / std::max(accuracy, 0.1f);
        const Vec3 way = iron::normalize(target - from);
        const Vec3 right = iron::normalize(iron::cross(way, Vec3{0.0f, 1.0f, 0.0f}));
        const Vec3 up = iron::cross(right, way);
        const auto gauss = [this] { return (random() + random() + random() - 1.5f) * 1.2f; };
        const Vec3 aimed = target + (right * gauss() + up * gauss()) * (spread * distance);
        shots_.push_back({from, iron::normalize(aimed - from), index(e)});
        e.soldier.flinch({0.0f, 0.03f});  // The rifle's kick.
        --e.burstLeft;
        e.shotIn = e.burstLeft > 0 ? 0.1f + 0.03f * random() : 0.55f + 0.9f * random();
    }

    // --- Cover ---
    int bestCover(const Enemy& e, Vec3 threat, bool flank) const {
        const Vec3 threatEye = threat + Vec3{0.0f, 1.5f, 0.0f};
        // The others' middle, for the flanker to get well round from.
        Vec3 squad{0.0f, 0.0f, 0.0f};
        int others = 0;
        for (const auto& o : enemies_) {
            if (!o->dead && o.get() != &e && o->perception.state().alerted) {
                squad = squad + o->at;
                ++others;
            }
        }
        if (others > 0) squad = squad * (1.0f / static_cast<float>(others));
        const Vec3 fromSquad = others > 0 ? level(squad - threat) : level(e.at - threat);
        struct Candidate {
            float score;
            int   index;
        };
        std::vector<Candidate> found;
        const int island = nav_.island(e.at);
        for (std::size_t i = 0; i < cover_.size(); ++i) {
            const CoverPoint& c = cover_[i];
            if (c.island != island) continue;  // Shut in somewhere he can't get to.
            if (c.takenBy >= 0 && c.takenBy != index(e)) continue;
            if (shunned(e, static_cast<int>(i))) continue;
            const float d = iron::length(c.at - e.at);
            if (d > (flank ? 34.0f : 26.0f)) continue;
            const Vec3 to = threat - c.at;
            const float td = std::sqrt(to.x * to.x + to.z * to.z);
            if (td < 6.0f || td > 40.0f) continue;
            // It must stand between him and the threat.
            if (iron::dot(c.out, to * (1.0f / td)) > -0.3f) continue;
            float score = d + 0.4f * std::abs(td - 14.0f);
            if (flank) {
                const float angle = std::acos(std::clamp(iron::dot(level(c.at - threat), fromSquad), -1.0f, 1.0f));
                if (angle < 0.9f) continue;
                score -= 10.0f * std::min(angle, 1.6f);
            }
            found.push_back({score, static_cast<int>(i)});
        }
        std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
        // The best that hides him from where the threat was.
        // ...and that he can get to.
        const int tries = std::min<int>(static_cast<int>(found.size()), 24);
        std::vector<Vec3> path;
        int seen = 0, unreachable = 0;
        for (int k = 0; k < tries; ++k) {
            const CoverPoint& c = cover_[found[k].index];
            const Vec3 hidden = c.at + Vec3{0.0f, c.low() ? 1.0f : 1.5f, 0.0f};
            if (senses_->visibility(threatEye, hidden) >= 0.15f) {
                ++seen;
                continue;
            }
            if (!nav_.findPath(e.at, c.at, path) || path.empty() || iron::length(path.back() - c.at) > 0.3f) {
                ++unreachable;
                continue;
            }
            return found[k].index;
        }
        if (log) {
            iron::Log::info("ai %-5s cover: %zu of %zu facing, %d seen, %d unreachable (threat %.1f %.1f)", e.name.c_str(),
                            found.size(), cover_.size(), seen, unreachable, threat.x, threat.z);
        }
        return -1;
    }

    Vec3 coverAt(const Enemy& e) const { return e.cover >= 0 ? cover_[e.cover].at : e.at; }

    // Cover that failed him (couldn't get there, or was seen in it): not again for a while.
    void shun(Enemy& e) {
        if (e.cover >= 0) e.shunned.push_back({e.cover, clock_});
        release(e);
    }
    bool shunned(const Enemy& e, int cover) const {
        return std::any_of(e.shunned.begin(), e.shunned.end(), [&](const std::pair<int, float>& s) {
            return s.first == cover && clock_ - s.second < 15.0f;
        });
    }

    void release(Enemy& e) {
        if (e.cover >= 0 && cover_[e.cover].takenBy == index(e)) cover_[e.cover].takenBy = -1;
        e.cover = -1;
    }

    // One of three or more in a fight goes round the side.
    void assignFlanker() {
        int alerted = 0;
        Enemy* flanker = nullptr;
        for (auto& e : enemies_) {
            if (e->dead || !e->perception.state().alerted) {
                e->flanker = e->flanked = false;
                continue;
            }
            ++alerted;
            if (e->flanker) flanker = e.get();
        }
        if (alerted < 3 || flanker) return;
        // The one furthest from where the player was, who has the most room to go round.
        float best = -1.0f;
        for (auto& e : enemies_) {
            if (e->dead || !e->perception.state().alerted || e->guard()) continue;
            const float d = iron::length(e->at - known_);
            if (d > best) {
                best = d;
                flanker = e.get();
            }
        }
        if (flanker) flanker->flanker = true;
    }

    int index(const Enemy& e) const {
        for (std::size_t i = 0; i < enemies_.size(); ++i) {
            if (enemies_[i].get() == &e) return static_cast<int>(i);
        }
        return -1;
    }

    static float wrap(float a) {
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    }
    static Vec3 level(Vec3 v) {
        const float l = std::sqrt(v.x * v.x + v.z * v.z);
        return l > 1e-4f ? Vec3{v.x / l, 0.0f, v.z / l} : Vec3{0.0f, 0.0f, -1.0f};
    }
    float random() {
        seed_ = seed_ * 1664525u + 1013904223u;
        return static_cast<float>(seed_ >> 8) * (1.0f / 16777216.0f);
    }

    iron::NavMesh&                       nav_;
    std::vector<CoverPoint>              cover_;
    std::vector<std::unique_ptr<Enemy>>  enemies_;
    const Senses*                        senses_ = nullptr;
    Vec3                                 known_{};
    float                                sinceSeen_ = 1.0e9f;
    float                                radioIn_ = -1.0f;
    Vec3                                 radioAt_{};
    std::uint32_t                        seed_ = 12345u;
    std::vector<Shot>                    shots_;
    std::vector<std::pair<Vec3, float>>  falling_;  // Bodies on their way down, and how long till they land.
    std::vector<Vec3>                    landed_;
    float                                clock_ = 0.0f;
};

}  // namespace foundry
