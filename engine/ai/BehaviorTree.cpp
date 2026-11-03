#include "ai/BehaviorTree.h"

namespace iron {

BtStatus BtNode::tick(BtContext& ctx) {
    if (!running_) {
        running_ = true;
        onStart(ctx);
    }
    const BtStatus status = onTick(ctx);
    if (status != BtStatus::Running) running_ = false;
    return status;
}

void BtNode::abort(BtContext& ctx) {
    if (!running_) return;
    onAbort(ctx);
    running_ = false;
}

void BtNode::runningPath(std::string& out) const { out += name_; }

namespace {

void pathThrough(const std::string& name, const BtNode* child, std::string& out) {
    out += name;
    if (child && child->running()) {
        out += " > ";
        child->runningPath(out);
    }
}

}  // namespace

// --- Selector ---

BtSelector::BtSelector(std::string name, std::vector<BtPtr> children)
    : BtNode(std::move(name)), children_(std::move(children)) {}

BtStatus BtSelector::onTick(BtContext& ctx) {
    // A higher-priority branch whose condition now holds takes over.
    if (current_ < children_.size() && children_[current_]->running()) {
        for (std::size_t i = 0; i < current_; ++i) {
            if (children_[i]->abortsLower() && children_[i]->canEnter(ctx)) {
                children_[current_]->abort(ctx);
                current_ = i;
                break;
            }
        }
    }
    while (current_ < children_.size()) {
        const BtStatus status = children_[current_]->tick(ctx);
        if (status != BtStatus::Failure) return status;
        ++current_;
    }
    return BtStatus::Failure;
}

void BtSelector::onAbort(BtContext& ctx) {
    if (current_ < children_.size()) children_[current_]->abort(ctx);
}

void BtSelector::runningPath(std::string& out) const {
    pathThrough(name(), current_ < children_.size() ? children_[current_].get() : nullptr, out);
}

// --- Sequence ---

BtSequence::BtSequence(std::string name, std::vector<BtPtr> children)
    : BtNode(std::move(name)), children_(std::move(children)) {}

BtStatus BtSequence::onTick(BtContext& ctx) {
    while (current_ < children_.size()) {
        const BtStatus status = children_[current_]->tick(ctx);
        if (status != BtStatus::Success) return status;
        ++current_;
    }
    return BtStatus::Success;
}

void BtSequence::onAbort(BtContext& ctx) {
    if (current_ < children_.size()) children_[current_]->abort(ctx);
}

void BtSequence::runningPath(std::string& out) const {
    pathThrough(name(), current_ < children_.size() ? children_[current_].get() : nullptr, out);
}

// --- Condition ---

BtCondition::BtCondition(std::string name, Predicate holds, BtAborts aborts, BtPtr child)
    : BtNode(std::move(name)), holds_(std::move(holds)), aborts_(aborts), child_(std::move(child)) {}

BtStatus BtCondition::onTick(BtContext& ctx) {
    // Checked on the way in, and every tick after if it aborts itself.
    const bool entering = !child_->running();
    const bool watching = aborts_ == BtAborts::Self || aborts_ == BtAborts::Both;
    if ((entering || watching) && !holds_(ctx.board)) {
        child_->abort(ctx);
        return BtStatus::Failure;
    }
    return child_->tick(ctx);
}

void BtCondition::onAbort(BtContext& ctx) { child_->abort(ctx); }

void BtCondition::runningPath(std::string& out) const { pathThrough(name(), child_.get(), out); }

// --- Leaves ---

BtAction::BtAction(std::string name, Tick tick, Start start, Abort abort)
    : BtNode(std::move(name)), tick_(std::move(tick)), start_(std::move(start)), abort_(std::move(abort)) {}

BtWait::BtWait(std::string name, float seconds, std::string key)
    : BtNode(std::move(name)), seconds_(seconds), key_(std::move(key)) {}

void BtWait::onStart(BtContext& ctx) { left_ = key_.empty() ? seconds_ : ctx.board.get<float>(key_, seconds_); }

BtStatus BtWait::onTick(BtContext& ctx) {
    left_ -= ctx.dt;
    return left_ <= 0.0f ? BtStatus::Success : BtStatus::Running;
}

}  // namespace iron
