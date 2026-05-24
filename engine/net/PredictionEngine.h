#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>

namespace iron {

// Client-side input history + reconciliation against authoritative
// server state. Game provides a deterministic simulate(state, input,
// dt) → state function. PredictionEngine handles bookkeeping:
//
//   - applyInput: advance predicted state, record (inputId, input,
//     predicted) in history. Returns the new monotonic inputId.
//
//   - predictedState(): the latest locally-predicted state.
//
//   - reconcile(authState, lastConfirmedInputId): server has applied
//     all inputs through lastConfirmedInputId and the result is
//     authState. Drop confirmed entries from history. If our prediction
//     at lastConfirmedInputId matches authState, predicted is already
//     consistent — keep it. If it doesn't (or that input is no longer
//     in history), snap predicted to authState and replay all surviving
//     history entries against it to bring "now" back up.
//
//   - reset(state): wipe history; set predicted; restart inputIds at 1.
//
// Stale reconciles (lastConfirmedInputId older than what we've already
// reconciled past) are ignored.
//
// Comparison uses TState::operator==. Works for deterministic
// single-platform sims where client and server run the same simulate
// on the same inputs.
template <typename TInput, typename TState>
class PredictionEngine {
public:
    using SimulateFn =
        std::function<TState(const TState&, const TInput&, float dtSec)>;

    PredictionEngine(SimulateFn simulate, float fixedDtSec,
                     TState initial = {})
        : simulate_(std::move(simulate)),
          fixedDt_(fixedDtSec),
          predicted_(std::move(initial)) {}

    std::uint32_t applyInput(const TInput& input) {
        const std::uint32_t id = nextInputId_++;
        predicted_ = simulate_(predicted_, input, fixedDt_);
        history_.push_back({id, input, predicted_});
        return id;
    }

    const TState& predictedState() const { return predicted_; }

    void reconcile(const TState& authState,
                   std::uint32_t lastConfirmedInputId) {
        if (history_.empty()) {
            predicted_ = authState;
            return;
        }
        if (lastConfirmedInputId < history_.front().inputId) {
            return;  // stale; already reconciled past
        }

        bool matched = false;
        auto match = std::find_if(history_.begin(), history_.end(),
            [&](const Entry& e) { return e.inputId == lastConfirmedInputId; });
        if (match != history_.end()) {
            matched = (match->predicted == authState);
        }

        while (!history_.empty() &&
               history_.front().inputId <= lastConfirmedInputId) {
            history_.pop_front();
        }

        if (matched) return;

        predicted_ = authState;
        for (auto& entry : history_) {
            predicted_ = simulate_(predicted_, entry.input, fixedDt_);
            entry.predicted = predicted_;
        }
    }

    std::size_t historySize() const { return history_.size(); }

    void reset(const TState& state) {
        history_.clear();
        predicted_ = state;
        nextInputId_ = 1;
    }

private:
    struct Entry {
        std::uint32_t inputId;
        TInput input;
        TState predicted;
    };

    SimulateFn simulate_;
    float fixedDt_;
    TState predicted_;
    std::uint32_t nextInputId_ = 1;
    std::deque<Entry> history_;
};

}  // namespace iron
