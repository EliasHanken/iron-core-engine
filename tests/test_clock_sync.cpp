#include "test_framework.h"
#include "net/ClockSync.h"

using namespace iron;

int main() {
    // ready() flips at kMinSamples.
    {
        ClockSync cs;
        CHECK(!cs.ready());
        cs.onPongReceived(0.0, 0.02, 100.01);
        cs.onPongReceived(1.0, 1.02, 101.01);
        CHECK(!cs.ready());
        cs.onPongReceived(2.0, 2.02, 102.01);
        CHECK(cs.ready());
    }

    // With three samples of perfect 20 ms RTT and 100 s offset, smoothed
    // rtt ≈ 0.02 and offset ≈ 100.00.
    {
        ClockSync cs;
        cs.onPongReceived(0.0, 0.02, 100.01);  // localMid=0.01, hostTime=100.01, offset=100.00
        cs.onPongReceived(1.0, 1.02, 101.01);
        cs.onPongReceived(2.0, 2.02, 102.01);
        CHECK_NEAR(static_cast<float>(cs.rttSec()), 0.02f);
        CHECK_NEAR(static_cast<float>(cs.offsetSec()), 100.0f);
    }

    // remoteTimeNow returns localNow + offset once ready.
    {
        ClockSync cs;
        cs.onPongReceived(0.0, 0.02, 100.01);
        cs.onPongReceived(1.0, 1.02, 101.01);
        cs.onPongReceived(2.0, 2.02, 102.01);
        CHECK_NEAR(static_cast<float>(cs.remoteTimeNow(50.0)), 150.0f);
    }

    return iron_test_result();
}
