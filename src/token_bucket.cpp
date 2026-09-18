#include "token_bucket.h"

#include <algorithm>
#include <stdexcept>

namespace rl {

TokenBucket::TokenBucket(double capacity, double refillRatePerSec, TimePoint now)
    : capacity_(capacity),
      refillRatePerSec_(refillRatePerSec),
      tokens_(capacity),
      lastRefill_(now) {
    if (capacity <= 0.0) throw std::invalid_argument("capacity must be > 0");
    if (refillRatePerSec < 0.0) throw std::invalid_argument("refill rate must be >= 0");
}

void TokenBucket::refill(TimePoint now) {
    if (now <= lastRefill_) return;  // no time passed (or clock went backwards): do nothing
    const std::chrono::duration<double> elapsed = now - lastRefill_;
    tokens_ = std::min(capacity_, tokens_ + elapsed.count() * refillRatePerSec_);
    lastRefill_ = now;
}

bool TokenBucket::tryConsume(TimePoint now, double cost) {
    refill(now);
    if (tokens_ >= cost) {
        tokens_ -= cost;
        return true;
    }
    return false;
}

}  // namespace rl