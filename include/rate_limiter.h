#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

#include "token_bucket.h"

namespace rl {

// Thread-safe, per-client rate limiter. Every client gets its own TokenBucket
// with the same capacity and refill rate. Safe to call from many threads.
class RateLimiter {
public:
    // capacity: max burst size (tokens). refillRatePerSec: sustained rate.
    // Throws std::invalid_argument on invalid configuration.
    RateLimiter(double capacity, double refillRatePerSec);

    // Returns true if the request is allowed (consumes one token), false if rejected.
    bool allowRequest(const std::string& clientId);

    // Number of clients that currently have a bucket.
    std::size_t clientCount() const;

private:
    // Immutable after construction, so they can be read without holding the lock.
    const double capacity_;
    const double refillRatePerSec_;

    // Guards buckets_: both the map structure (inserts may rehash) and every
    // TokenBucket stored in it (tryConsume mutates tokens_ and lastRefill_).
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TokenBucket> buckets_;
};

}  // namespace rl