#pragma once

#include <array>
#include <cstddef>

namespace iron {

// Client-side estimator of (host clock - local clock) and round-trip
// time. Fed by pong samples; reports the median of the last N
// (default 8) for robustness against latency spikes. `ready()` flips
// to true once at least kMinSamples pongs have been processed.
class ClockSync {
public:
    static constexpr int kSamples    = 8;
    static constexpr int kMinSamples = 3;

    void onPongReceived(double localSendTimeSec, double localRecvTimeSec,
                        double remoteTimeSec);

    bool   ready() const { return count_ >= kMinSamples; }
    double rttSec() const;       // smoothed; 0 if !ready()
    double offsetSec() const;    // hostTime - localTime, rtt/2 baked in; 0 if !ready()

    // Convenience: given local "now", return the estimated host-clock
    // value at this instant.
    double remoteTimeNow(double localNowSec) const {
        return localNowSec + offsetSec();
    }

private:
    struct Sample { double rtt; double offset; };
    std::array<Sample, kSamples> ring_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

}  // namespace iron
