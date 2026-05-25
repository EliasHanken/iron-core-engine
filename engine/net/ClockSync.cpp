#include "net/ClockSync.h"

#include <algorithm>
#include <vector>

namespace iron {

void ClockSync::onPongReceived(double localSendTimeSec, double localRecvTimeSec,
                               double remoteTimeSec) {
    const double rtt = std::max(0.0, localRecvTimeSec - localSendTimeSec);
    // remoteTime was sampled approximately mid-flight. Estimate localTime
    // at that mid-flight instant and compute offset.
    const double localMid = localSendTimeSec + rtt * 0.5;
    const double offset = remoteTimeSec - localMid;

    ring_[next_] = Sample{rtt, offset};
    next_ = (next_ + 1) % kSamples;
    if (count_ < kSamples) ++count_;
}

static double medianOf(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double ClockSync::rttSec() const {
    if (!ready()) return 0.0;
    std::vector<double> v;
    v.reserve(count_);
    for (std::size_t i = 0; i < count_; ++i) v.push_back(ring_[i].rtt);
    return medianOf(v);
}

double ClockSync::offsetSec() const {
    if (!ready()) return 0.0;
    std::vector<double> v;
    v.reserve(count_);
    for (std::size_t i = 0; i < count_; ++i) v.push_back(ring_[i].offset);
    return medianOf(v);
}

}  // namespace iron
