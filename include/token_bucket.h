#pragma once

#include <chrono>

namespace rl {

// A single token bucket. NOT thread-safe on its own: the owner (RateLimiter)
// is responsible for serializing access. Time is passed in explicitly so the
// refill math is deterministic and easy to unit test.
class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // Starts full. Throws std::invalid_argument if capacity <= 0 or refillRatePerSec < 0.
    TokenBucket(double capacity, double refillRatePerSec, TimePoint now);

    // Lazily refills based on elapsed time, then tries to remove `cost` tokens.
    bool tryConsume(TimePoint now, double cost = 1.0);

    double tokens() const { return tokens_; }
    double capacity() const { return capacity_; }
    TimePoint lastRefill() const { return lastRefill_; }

private:
    void refill(TimePoint now);

    double capacity_;
    double refillRatePerSec_;
    double tokens_;
    TimePoint lastRefill_;
};

}  // namespace rl