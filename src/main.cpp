#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rate_limiter.h"

namespace {

// Serializes console output so lines from different threads never interleave.
class Logger {
public:
    void log(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << line << '\n';
    }

private:
    std::mutex mutex_;
};

// How one simulated client behaves.
struct ClientProfile {
    std::string name;
    int requests;
    std::chrono::milliseconds gapBetweenRequests;
};

struct ClientResult {
    int allowed = 0;
    int rejected = 0;
};

}  // namespace

int main() {
    constexpr double kCapacity = 5.0;         // max burst
    constexpr double kRefillPerSecond = 2.0;  // sustained rate

    using std::chrono::milliseconds;
    const std::vector<ClientProfile> clients = {
        {"A", 20, milliseconds(100)},  // ~10 req/s: faster than the limit
        {"B", 20, milliseconds(0)},    // sends everything at once
        {"C", 8, milliseconds(600)},   // ~1.7 req/s: within the limit
    };

    rl::RateLimiter limiter(kCapacity, kRefillPerSecond);
    Logger logger;

    std::cout << "Rate limiter demo: capacity=" << kCapacity
              << " tokens, refill=" << kRefillPerSecond << " tokens/sec\n\n";

    // Each thread writes only to its own results[i], so no locking is needed
    // for it; the main thread reads results only after join().
    std::vector<ClientResult> results(clients.size());
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < clients.size(); ++i) {
        threads.emplace_back([&, i] {
            const ClientProfile& profile = clients[i];
            const std::string clientId = "client_" + profile.name;
            for (int n = 1; n <= profile.requests; ++n) {
                const bool allowed = limiter.allowRequest(clientId);
                (allowed ? results[i].allowed : results[i].rejected)++;
                logger.log("Client " + profile.name + " | Request " + std::to_string(n) +
                           " | " + (allowed ? "ALLOWED" : "REJECTED"));
                std::this_thread::sleep_for(profile.gapBetweenRequests);
            }
        });
    }
    for (auto& t : threads) t.join();

    std::cout << "\nSummary\n";
    for (std::size_t i = 0; i < clients.size(); ++i) {
        std::cout << "Client " << clients[i].name << ": " << results[i].allowed
                  << " allowed, " << results[i].rejected << " rejected (of "
                  << clients[i].requests << ")\n";
    }
    std::cout << "Tracked clients: " << limiter.clientCount() << '\n';
    return 0;
}