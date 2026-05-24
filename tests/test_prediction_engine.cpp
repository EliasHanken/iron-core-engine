#include "test_framework.h"
#include "net/PredictionEngine.h"

using namespace iron;

int main() {
    auto sim = [](const int& s, const int& i, float) -> int { return s + i; };

    // Case 1: applyInput accumulates predicted state and history.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        const auto id1 = e.applyInput(1); CHECK(id1 == 1);
        const auto id2 = e.applyInput(2); CHECK(id2 == 2);
        const auto id3 = e.applyInput(3); CHECK(id3 == 3);
        CHECK(e.predictedState() == 6);
        CHECK(e.historySize() == 3);
    }

    // Case 2: reconcile match → drops confirmed, predicted unchanged.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        e.applyInput(1);    // predicted = 1
        e.applyInput(2);    // predicted = 3
        e.applyInput(3);    // predicted = 6
        e.reconcile(3, /*lastConfirmedInputId*/2);
        CHECK(e.predictedState() == 6);
        CHECK(e.historySize() == 1);
    }

    // Case 3: reconcile mismatch → snap + replay.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        e.applyInput(1);    // 1
        e.applyInput(2);    // 3
        e.applyInput(3);    // 6
        e.reconcile(100, /*lastConfirmedInputId*/2);
        CHECK(e.predictedState() == 103);  // 100 + 3
        CHECK(e.historySize() == 1);
    }

    // Case 4: stale reconcile → ignored.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        e.applyInput(1); e.applyInput(2); e.applyInput(3);  // predicted = 6
        e.reconcile(3, 2);  // history [input 3], predicted = 6
        const int before = e.predictedState();
        const std::size_t hSize = e.historySize();
        e.reconcile(999, 1);  // stale
        CHECK(e.predictedState() == before);
        CHECK(e.historySize() == hSize);
    }

    // Case 5: reset wipes history and sets predicted; inputId restarts.
    {
        PredictionEngine<int, int> e{sim, 0.033f, /*initial*/0};
        e.applyInput(1); e.applyInput(2);
        CHECK(e.predictedState() == 3);
        CHECK(e.historySize() == 2);
        e.reset(50);
        CHECK(e.predictedState() == 50);
        CHECK(e.historySize() == 0);
        const auto id = e.applyInput(7);
        CHECK(id == 1);
        CHECK(e.predictedState() == 57);
    }

    return iron_test_result();
}
