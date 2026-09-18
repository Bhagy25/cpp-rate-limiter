#include "rate_limiter.h"

#include <stdexcept>

namespace rl {

RateLimiter::RateLimiter(double capacity, double refillRatePerSec)
    : capacity_(capacity), refillRatePerSec_(refillRatePerSec) {
    if (capacity <= 0.0) throw std::invalid_argument("capacity must be > 0");
    if (refillRatePerSec < 0.0) throw std::invalid_argument("refill rate must be >= 0");
}

bool RateLimiter::allowRequest(const std::string& clientId) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Read the clock while holding the lock so timestamps are observed in the
    // same order the lock is acquired.
    const auto now = TokenBucket::Clock::now();

    // try_emplace constructs a new bucket only if clientId is not present.
    // (operator[] would need a default-constructible TokenBucket.)
    auto it = buckets_.try_emplace(clientId, capacity_, refillRatePerSec_, now).first;
    return it->second.tryConsume(now);
}

std::size_t RateLimiter::clientCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buckets_.size();
}

}  // namespace rl