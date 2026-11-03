#pragma once

// A behaviour tree, the way Unreal's works: composites (selector, sequence) over
// tasks, with conditions as decorators that watch the blackboard and abort what's
// running when they change. Each tick walks down from the root to the running
// task; a task can take many ticks (Running) before it succeeds or fails.
//
// Conditions abort like Unreal's observer aborts:
//   Self           while its branch runs, it fails the moment the condition does;
//   LowerPriority  while a later sibling under the same selector runs, its branch
//                  takes over the moment the condition holds;
//   Both           both.
// So "if alerted, fight" placed first under the root selector, aborting Both,
// cuts any patrol short the moment the soldier's alerted, and the fight off the
// moment he isn't.
//
// A tree here is one agent's: nodes keep their own running state (which child,
// how long waited). Build one per agent from a function.

#include "math/Vec.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iron {

// What an agent knows, by name: what perception saw, where it's going, whom it's after.
class Blackboard {
public:
    using Value = std::variant<bool, int, float, Vec3>;

    void set(const std::string& key, Value value) { values_[key] = value; }
    void clear(const std::string& key) { values_.erase(key); }
    bool has(const std::string& key) const { return values_.count(key) != 0; }

    // The value under `key` if it holds a T, else `fallback`.
    template <typename T>
    T get(const std::string& key, T fallback = T{}) const {
        const auto found = values_.find(key);
        if (found == values_.end()) return fallback;
        const T* v = std::get_if<T>(&found->second);
        return v ? *v : fallback;
    }

private:
    std::unordered_map<std::string, Value> values_;
};

enum class BtStatus { Running, Success, Failure };

struct BtContext {
    Blackboard& board;
    float       dt = 0.0f;
};

class BtNode {
public:
    explicit BtNode(std::string name) : name_(std::move(name)) {}
    virtual ~BtNode() = default;

    // Runs a tick; first call after a finish or an abort starts it afresh.
    BtStatus tick(BtContext& ctx);
    // Stops it mid-run (it was Running), letting it tidy up.
    void abort(BtContext& ctx);
    bool running() const { return running_; }
    const std::string& name() const { return name_; }

    // For a condition: whether it holds now (others always may be entered).
    virtual bool canEnter(const BtContext&) const { return true; }
    // For a condition aborting LowerPriority: whether to watch it while a later sibling runs.
    virtual bool abortsLower() const { return false; }

    // The names down the running path, root first ("Root > Fight > TakeCover").
    virtual void runningPath(std::string& out) const;

protected:
    virtual void onStart(BtContext&) {}
    virtual BtStatus onTick(BtContext& ctx) = 0;
    virtual void onAbort(BtContext&) {}

private:
    std::string name_;
    bool        running_ = false;
};

using BtPtr = std::unique_ptr<BtNode>;

// Tries its children in order; the first not to fail decides. Each tick it also
// checks the conditions before the running child that abort LowerPriority.
class BtSelector : public BtNode {
public:
    BtSelector(std::string name, std::vector<BtPtr> children);
    void runningPath(std::string& out) const override;

protected:
    void onStart(BtContext&) override { current_ = 0; }
    BtStatus onTick(BtContext& ctx) override;
    void onAbort(BtContext& ctx) override;

private:
    std::vector<BtPtr> children_;
    std::size_t        current_ = 0;
};

// Runs its children in order while each succeeds; the first to fail fails it.
class BtSequence : public BtNode {
public:
    BtSequence(std::string name, std::vector<BtPtr> children);
    void runningPath(std::string& out) const override;

protected:
    void onStart(BtContext&) override { current_ = 0; }
    BtStatus onTick(BtContext& ctx) override;
    void onAbort(BtContext& ctx) override;

private:
    std::vector<BtPtr> children_;
    std::size_t        current_ = 0;
};

enum class BtAborts { None, Self, LowerPriority, Both };

// A decorator: its child runs only while `holds` does (see the aborts above).
class BtCondition : public BtNode {
public:
    using Predicate = std::function<bool(const Blackboard&)>;
    BtCondition(std::string name, Predicate holds, BtAborts aborts, BtPtr child);
    bool canEnter(const BtContext& ctx) const override { return holds_(ctx.board); }
    bool abortsLower() const override { return aborts_ == BtAborts::LowerPriority || aborts_ == BtAborts::Both; }
    void runningPath(std::string& out) const override;

protected:
    BtStatus onTick(BtContext& ctx) override;
    void onAbort(BtContext& ctx) override;

private:
    Predicate holds_;
    BtAborts  aborts_;
    BtPtr     child_;
};

// A task, from functions: start (optional), tick (required), abort (optional).
class BtAction : public BtNode {
public:
    using Start = std::function<void(BtContext&)>;
    using Tick = std::function<BtStatus(BtContext&)>;
    using Abort = std::function<void(BtContext&)>;
    BtAction(std::string name, Tick tick, Start start = {}, Abort abort = {});

protected:
    void onStart(BtContext& ctx) override {
        if (start_) start_(ctx);
    }
    BtStatus onTick(BtContext& ctx) override { return tick_(ctx); }
    void onAbort(BtContext& ctx) override {
        if (abort_) abort_(ctx);
    }

private:
    Tick  tick_;
    Start start_;
    Abort abort_;
};

// Waits `seconds` (a blackboard float key's value, if `key` is set, read on start).
class BtWait : public BtNode {
public:
    BtWait(std::string name, float seconds, std::string key = {});

protected:
    void onStart(BtContext& ctx) override;
    BtStatus onTick(BtContext& ctx) override;

private:
    float       seconds_;
    std::string key_;
    float       left_ = 0.0f;
};

// Shorthands for building trees in code.
namespace bt {

template <typename... Nodes>
std::vector<BtPtr> list(Nodes&&... nodes) {
    std::vector<BtPtr> out;
    (out.push_back(std::forward<Nodes>(nodes)), ...);
    return out;
}
template <typename... Nodes>
BtPtr selector(std::string name, Nodes&&... children) {
    return std::make_unique<BtSelector>(std::move(name), list(std::forward<Nodes>(children)...));
}
template <typename... Nodes>
BtPtr sequence(std::string name, Nodes&&... children) {
    return std::make_unique<BtSequence>(std::move(name), list(std::forward<Nodes>(children)...));
}
inline BtPtr when(std::string name, BtCondition::Predicate holds, BtAborts aborts, BtPtr child) {
    return std::make_unique<BtCondition>(std::move(name), std::move(holds), aborts, std::move(child));
}
inline BtPtr action(std::string name, BtAction::Tick tick, BtAction::Start start = {}, BtAction::Abort abort = {}) {
    return std::make_unique<BtAction>(std::move(name), std::move(tick), std::move(start), std::move(abort));
}
inline BtPtr wait(std::string name, float seconds, std::string key = {}) {
    return std::make_unique<BtWait>(std::move(name), seconds, std::move(key));
}

}  // namespace bt

}  // namespace iron
