// Minimal self-contained test runner (no external framework).
// Deterministic tests pass explicit time points to TokenBucket, so they need no sleeping.

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "rate_limiter.h"
#include "token_bucket.h"

namespace {

using std::chrono::milliseconds;
using rl::RateLimiter;
using rl::TokenBucket;

int g_failedChecks = 0;

// Starts `threadCount` threads that all wait on a gate and are released together,
// so they genuinely overlap instead of running one after another.
void runConcurrently(int threadCount, const std::function<void()>& work) {
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            work();
        });
    }
    go.store(true);
    for (auto& th : threads) th.join();
}

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::cout << "    check failed (line " << __LINE__ << "): " #condition "\n"; \
            ++g_failedChecks;                                                         \
        }                                                                             \
    } while (0)

// Test 1: a new client can consume exactly its capacity.
void testInitialCapacity() {
    RateLimiter limiter(5, 0);  // refill 0: only the initial tokens exist
    for (int i = 0; i < 5; ++i) CHECK(limiter.allowRequest("client_1"));
}

// Test 2: once tokens are exhausted, requests are rejected.
void testRejectionAfterExhaustion() {
    RateLimiter limiter(3, 0);
    for (int i = 0; i < 3; ++i) CHECK(limiter.allowRequest("client_1"));
    CHECK(!limiter.allowRequest("client_1"));
    CHECK(!limiter.allowRequest("client_1"));
}

// Test 3a: refill over time (deterministic, explicit timestamps).
void testRefillDeterministic() {
    const TokenBucket::TimePoint t0{};
    TokenBucket bucket(5, 2, t0);  // 5 tokens, +2 per second
    for (int i = 0; i < 5; ++i) CHECK(bucket.tryConsume(t0));
    CHECK(!bucket.tryConsume(t0));

    const auto t1 = t0 + milliseconds(1000);  // 1s * 2/s = 2 tokens
    CHECK(bucket.tryConsume(t1));
    CHECK(bucket.tryConsume(t1));
    CHECK(!bucket.tryConsume(t1));
}

// Test 3b: refill with the real clock, end to end through RateLimiter.
void testRefillRealClock() {
    RateLimiter limiter(1, 10);  // 1 token, +10/sec => 1 token per 100 ms
    CHECK(limiter.allowRequest("client_1"));
    CHECK(!limiter.allowRequest("client_1"));
    std::this_thread::sleep_for(milliseconds(250));  // generous margin over 100 ms
    CHECK(limiter.allowRequest("client_1"));
}

// Test 3c: tokens never exceed capacity, no matter how long the client was idle.
void testRefillCappedAtCapacity() {
    const TokenBucket::TimePoint t0{};
    TokenBucket bucket(5, 2, t0);
    CHECK(bucket.tryConsume(t0));  // 4 left

    const auto later = t0 + std::chrono::hours(1);
    for (int i = 0; i < 5; ++i) CHECK(bucket.tryConsume(later));
    CHECK(!bucket.tryConsume(later));  // capped at 5, not 7204
}

// Test 4: one client exhausting its bucket does not affect another.
void testClientIsolation() {
    RateLimiter limiter(3, 0);
    for (int i = 0; i < 3; ++i) CHECK(limiter.allowRequest("A"));
    CHECK(!limiter.allowRequest("A"));

    for (int i = 0; i < 3; ++i) CHECK(limiter.allowRequest("B"));
    CHECK(!limiter.allowRequest("B"));
    CHECK(limiter.clientCount() == 2);
}

// Test 5a: many threads hammer ONE client. Exactly `capacity` requests may succeed.
void testConcurrentSameClient() {
    constexpr int kThreads = 8;
    constexpr int kRequestsPerThread = 1000;
    constexpr int kCapacity = 100;

    RateLimiter limiter(kCapacity, 0);
    std::atomic<int> allowed{0};
    runConcurrently(kThreads, [&] {
        for (int i = 0; i < kRequestsPerThread; ++i) {
            if (limiter.allowRequest("shared")) ++allowed;
            std::this_thread::yield();
        }
    });
    CHECK(allowed.load() == kCapacity);
}

// Test 5b: many threads race to create and use MANY clients (map inserts / rehashing).
void testConcurrentManyClients() {
    constexpr int kThreads = 8;
    constexpr int kClients = 200;
    constexpr int kAttemptsPerClientPerThread = 5;
    constexpr int kCapacity = 3;

    RateLimiter limiter(kCapacity, 0);
    std::atomic<int> allowed{0};
    runConcurrently(kThreads, [&] {
        for (int c = 0; c < kClients; ++c) {
            const std::string id = "client_" + std::to_string(c);
            for (int a = 0; a < kAttemptsPerClientPerThread; ++a)
                if (limiter.allowRequest(id)) ++allowed;
            std::this_thread::yield();
        }
    });

    CHECK(allowed.load() == kClients * kCapacity);  // each client: exactly 3
    CHECK(limiter.clientCount() == static_cast<std::size_t>(kClients));
}

// Test 6a: refill boundaries and fractional elapsed time (deterministic).
// 0.5 s * 2 tokens/s = exactly 1.0, and 0.5 is exactly representable as a double.
void testBoundaryBehavior() {
    const TokenBucket::TimePoint t0{};
    TokenBucket bucket(5, 2, t0);
    for (int i = 0; i < 5; ++i) CHECK(bucket.tryConsume(t0));  // drain

    CHECK(!bucket.tryConsume(t0 + milliseconds(499)));  // 0.998 tokens: not enough
    CHECK(bucket.tryConsume(t0 + milliseconds(500)));   // exactly 1.0: allowed
    CHECK(!bucket.tryConsume(t0 + milliseconds(500)));  // just spent it
}

// Test 6b: rejected requests still credit elapsed time, so fractions accumulate.
void testFractionalAccumulation() {
    const TokenBucket::TimePoint t0{};
    TokenBucket bucket(5, 2, t0);
    for (int i = 0; i < 5; ++i) CHECK(bucket.tryConsume(t0));

    CHECK(!bucket.tryConsume(t0 + milliseconds(250)));  // 0.5 tokens
    CHECK(bucket.tryConsume(t0 + milliseconds(500)));   // 0.5 + 0.5 = 1.0
}

// Test 6c: zero refill rate never refills; a clock going backwards adds nothing.
void testEdgeCases() {
    const TokenBucket::TimePoint t0{};
    TokenBucket noRefill(1, 0, t0);
    CHECK(noRefill.tryConsume(t0));
    CHECK(!noRefill.tryConsume(t0 + std::chrono::hours(24)));

    TokenBucket bucket(5, 2, t0 + milliseconds(1000));
    for (int i = 0; i < 5; ++i) CHECK(bucket.tryConsume(t0 + milliseconds(1000)));
    CHECK(!bucket.tryConsume(t0));  // earlier timestamp: no negative refill, no free tokens
}

// Configuration validation.
void testInvalidConfiguration() {
    bool threw = false;
    try { RateLimiter limiter(0, 1); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    threw = false;
    try { RateLimiter limiter(5, -1); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

}  // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"Test 1  initial capacity", testInitialCapacity},
        {"Test 2  rejection after exhaustion", testRejectionAfterExhaustion},
        {"Test 3a refill (deterministic time)", testRefillDeterministic},
        {"Test 3b refill (real clock)", testRefillRealClock},
        {"Test 3c refill capped at capacity", testRefillCappedAtCapacity},
        {"Test 4  client isolation", testClientIsolation},
        {"Test 5a concurrent: one shared client", testConcurrentSameClient},
        {"Test 5b concurrent: many clients", testConcurrentManyClients},
        {"Test 6a boundary at exact refill point", testBoundaryBehavior},
        {"Test 6b fractional accumulation", testFractionalAccumulation},
        {"Test 6c zero refill / backwards clock", testEdgeCases},
        {"Test 7  invalid configuration", testInvalidConfiguration},
    };

    int failedTests = 0;
    for (const auto& test : tests) {
        const int failedBefore = g_failedChecks;
        bool threw = false;
        try {
            test.run();
        } catch (const std::exception& e) {
            std::cout << "    unexpected exception: " << e.what() << '\n';
            threw = true;
        }
        const bool passed = !threw && g_failedChecks == failedBefore;
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        if (!passed) ++failedTests;
    }

    std::cout << '\n'
              << (tests.size() - failedTests) << "/" << tests.size() << " tests passed\n";
    return failedTests == 0 ? 0 : 1;
}