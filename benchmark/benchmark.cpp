// Benchmark for rl::RateLimiter.
//
// Every scenario runs kRuns times; the run with the median elapsed time is reported.
// "Avg latency" is wall time * threads / total calls, i.e. the average time one
// thread spends per allowRequest() call, INCLUDING time spent waiting for the lock.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "rate_limiter.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kCapacity = 1000.0;
constexpr double kRefillPerSecond = 1000.0;
constexpr std::uint64_t kTotalRequests = 1'000'000;
constexpr int kRuns = 3;

struct Scenario {
    std::string name;
    std::size_t clients;
    int threads;
};

struct Result {
    int threads = 0;
    std::uint64_t total = 0;
    std::uint64_t allowed = 0;
    double elapsedSeconds = 0.0;

    std::uint64_t rejected() const { return total - allowed; }
    double throughputPerSecond() const { return static_cast<double>(total) / elapsedSeconds; }
    double avgLatencyNs() const {
        return elapsedSeconds * 1e9 * threads / static_cast<double>(total);
    }
};

Result runOnce(const Scenario& scenario) {
    rl::RateLimiter limiter(kCapacity, kRefillPerSecond);

    // Build client ids up front so string construction is not part of the timing.
    std::vector<std::string> ids;
    ids.reserve(scenario.clients);
    for (std::size_t i = 0; i < scenario.clients; ++i) ids.push_back("client_" + std::to_string(i));

    // Warm-up: create every bucket first, so the timed section measures steady-state
    // lookups rather than first-time inserts and rehashing.
    for (const auto& id : ids) limiter.allowRequest(id);

    const std::uint64_t perThread = kTotalRequests / static_cast<std::uint64_t>(scenario.threads);
    std::vector<std::uint64_t> allowedPerThread(static_cast<std::size_t>(scenario.threads), 0);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < scenario.threads; ++t) {
        threads.emplace_back([&, t] {
            std::uint64_t allowed = 0;
            std::size_t index = (static_cast<std::size_t>(t) * 7919) % ids.size();
            ++ready;
            while (!go.load()) std::this_thread::yield();

            for (std::uint64_t i = 0; i < perThread; ++i) {
                if (limiter.allowRequest(ids[index])) ++allowed;
                if (++index == ids.size()) index = 0;
            }
            allowedPerThread[static_cast<std::size_t>(t)] = allowed;
        });
    }

    while (ready.load() < scenario.threads) std::this_thread::yield();
    const auto start = Clock::now();
    go.store(true);
    for (auto& th : threads) th.join();
    const auto end = Clock::now();

    Result result;
    result.threads = scenario.threads;
    result.total = perThread * static_cast<std::uint64_t>(scenario.threads);
    for (std::uint64_t a : allowedPerThread) result.allowed += a;
    result.elapsedSeconds = std::chrono::duration<double>(end - start).count();
    return result;
}

Result runMedian(const Scenario& scenario) {
    std::vector<Result> runs;
    for (int i = 0; i < kRuns; ++i) runs.push_back(runOnce(scenario));
    std::sort(runs.begin(), runs.end(),
              [](const Result& a, const Result& b) { return a.elapsedSeconds < b.elapsedSeconds; });
    return runs[runs.size() / 2];
}

void printEnvironment() {
    std::cout << "Environment\n";
#if defined(__VERSION__)
    std::cout << "  Compiler:            " << __VERSION__ << '\n';
#elif defined(_MSC_VER)
    std::cout << "  Compiler:            MSVC " << _MSC_VER << '\n';
#endif
    std::cout << "  C++ standard:        " << __cplusplus << '\n';
#ifdef NDEBUG
    std::cout << "  Optimized build:     yes (NDEBUG defined)\n";
#else
    std::cout << "  Optimized build:     NO (numbers will be misleading; build in Release)\n";
#endif
    std::cout << "  Hardware threads:    " << std::thread::hardware_concurrency() << '\n';
    std::cout << "  Limiter config:      capacity=" << kCapacity << ", refill=" << kRefillPerSecond
              << "/s per client\n";
    std::cout << "  Requests/scenario:   " << kTotalRequests << " (median of " << kRuns << " runs)\n\n";
}

}  // namespace

int main() {
    printEnvironment();

    const std::vector<Scenario> scenarios = {
        {"1 client,     1 thread ", 1, 1},       {"10k clients,  1 thread ", 10000, 1},
        {"1 client,     2 threads", 1, 2},       {"1 client,     4 threads", 1, 4},
        {"1 client,     8 threads", 1, 8},       {"10k clients,  2 threads", 10000, 2},
        {"10k clients,  4 threads", 10000, 4},   {"10k clients,  8 threads", 10000, 8},
    };

    std::cout << std::left << std::setw(25) << "Scenario" << std::right << std::setw(10) << "Total"
              << std::setw(10) << "Allowed" << std::setw(10) << "Rejected" << std::setw(12)
              << "Time (ms)" << std::setw(14) << "M req/s" << std::setw(14) << "ns/call" << '\n';

    for (const auto& scenario : scenarios) {
        const Result r = runMedian(scenario);
        std::cout << std::left << std::setw(25) << scenario.name << std::right << std::setw(10)
                  << r.total << std::setw(10) << r.allowed << std::setw(10) << r.rejected()
                  << std::setw(12) << std::fixed << std::setprecision(1) << r.elapsedSeconds * 1e3
                  << std::setw(14) << std::setprecision(2) << r.throughputPerSecond() / 1e6
                  << std::setw(14) << std::setprecision(1) << r.avgLatencyNs() << '\n';
    }
    return 0;
}