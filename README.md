# High-Performance C++ Rate Limiter

A thread-safe, per-client rate limiter written in C++17, using the **token bucket** algorithm.
Inspired by the "Design a Rate Limiter" chapter of *System Design Interview* (ByteByteGo).

## Overview

`RateLimiter::allowRequest(clientId)` answers one question: may this client make a request
right now? Each client gets an independent token bucket that holds up to `capacity` tokens and
refills continuously at `refillRatePerSec`. A request consumes one token; with no token available it is
rejected. Many threads may call `allowRequest` at the same time.

```cpp
rl::RateLimiter limiter(5, 2);   // burst of 5, sustained 2 requests/second

if (limiter.allowRequest("client_1")) {
    // allowed
} else {
    // rejected
}
```

## Features

- Token bucket with **lazy refill** (no background thread)
- **Per-client** buckets: one client exhausting its quota does not affect others
- **Thread-safe** for concurrent callers (mutex-protected shared state)
- Time-injectable `TokenBucket`, so refill logic is unit-tested deterministically
- Concurrent multi-client demo with a thread-safe logger
- 12 automated tests (run through CTest)
- Benchmark reporting throughput and average latency
- C++17, CMake, no third-party dependencies

## Architecture

```text
            Incoming request (clientId)
                       |
                       v
              +------------------+
              |   RateLimiter    |   one std::mutex guards everything below
              +------------------+
                       |
                       v
              +------------------+
              |  unordered_map   |   clientId -> TokenBucket
              +------------------+
                       |
                       v
              +------------------+
              |   TokenBucket    |   refill from elapsed time, then try to consume 1 token
              +------------------+
                  |          |
               token      no token
                  |          |
               ALLOW       REJECT
```

| File | Responsibility |
|---|---|
| `include/token_bucket.h`, `src/token_bucket.cpp` | One bucket: capacity, tokens, refill rate, last refill time. Not thread-safe on its own. |
| `include/rate_limiter.h`, `src/rate_limiter.cpp` | Client-to-bucket map plus the mutex. The only public API most callers need. |
| `src/main.cpp` | Concurrent demo with a thread-safe logger. |
| `tests/test_rate_limiter.cpp` | Self-contained test runner. |
| `benchmark/benchmark.cpp` | Throughput and latency measurements. |

The core library (`ratelimiter`) has no dependency on the demo, tests or benchmark, and none on HTTP.

## Token Bucket Algorithm

State per client: `capacity`, `tokens`, `refillRatePerSec`, `lastRefill`.

On each request at time `now`:

```text
elapsed = now - lastRefill                      (seconds)
tokens  = min(capacity, tokens + elapsed * refillRatePerSec)
lastRefill = now
if tokens >= 1:  tokens -= 1;  ALLOW
else:                          REJECT
```

Example with `capacity = 5`, `refill = 2 tokens/s`:

| Time | Event | Tokens before | Result | Tokens after |
|---|---|---|---|---|
| 0.0 s | 5 requests | 5 | 5 x ALLOWED | 0 |
| 0.0 s | 6th request | 0 | REJECTED | 0 |
| 0.4 s | request | 0.8 | REJECTED | 0.8 |
| 0.5 s | request | 1.0 | ALLOWED | 0 |

Fractional tokens are kept, so refill is smooth rather than in whole-second steps. Refill is computed **lazily**
when a request arrives, so idle clients cost no CPU.

## Concurrency

- A single `std::mutex` in `RateLimiter` guards **both** the `unordered_map` (an insert can rehash and
  invalidate iterators) **and** every `TokenBucket` in it (`tryConsume` is a read-modify-write on
  `tokens_` and `lastRefill_`).
- `allowRequest` takes the lock with `std::lock_guard`, reads the clock while holding it, finds or creates the
  bucket with `try_emplace`, and consumes a token, all in one critical section.
- `capacity_` and `refillRatePerSec_` are `const` after construction, so they are read without a lock.
- **Atomics are not used in the core.** The operation "check tokens, subtract, update timestamp, maybe
  insert into the map" spans several variables, so individual atomic operations would not make it correct.
  `std::atomic` is used only in the tests and benchmark (result counters, start gate).
- `TokenBucket` takes the current time as a parameter and uses `std::chrono::steady_clock` in
  `RateLimiter` (monotonic, unaffected by system clock changes). A timestamp earlier than `lastRefill` is
  ignored, so tokens never go backwards.

## Design Decisions

| Decision | Reason | Alternative |
|---|---|---|
| Token bucket | Allows short bursts up to `capacity` while enforcing an average rate; O(1) state per client | Fixed window (boundary bursts), sliding window (more memory/accuracy), leaky bucket (smooths output, no bursts) |
| Lazy refill | No timer thread, no wasted work for idle clients | Background refill thread |
| `unordered_map` | Average O(1) lookup by client id | `std::map` (O(log n), ordered), sharded map (see Future Improvements) |
| One global mutex | Simple and obviously correct; measured limitation below | Per-shard or per-bucket locks |
| `steady_clock` | Monotonic time | `system_clock` can jump backwards |
| Per-client buckets | Isolation between clients | One shared bucket |

## Complexity

- **Time per request:** O(1) average for the hash lookup plus refill arithmetic. Hashing the client id costs
  O(length of id). Worst-case hash lookup is O(n) with pathological collisions, and an insert that triggers a
  rehash is O(n) (amortized O(1)).
- **Space:** O(number of clients that have made a request). Buckets are never evicted in this version.

## Build Instructions

Requirements: a C++17 compiler (tested with GCC 16.2.0 from MSYS2 on Windows and GCC 13 on Linux), CMake 3.14+.

```bash
cmake -S . -B build -G Ninja      # -G Ninja is optional; omit it on Linux if you prefer Makefiles
cmake --build build
```

## Run Instructions

```bash
# Windows (PowerShell)
.\build\rate_limiter_demo.exe
# Linux / macOS
./build/rate_limiter_demo
```

## Testing

```bash
ctest --test-dir build --output-on-failure
# or run the test binary directly for per-test output:
./build/test_rate_limiter          # Windows: .\build\test_rate_limiter.exe
```

Twelve tests cover: initial capacity, rejection, refill (deterministic and real clock), refill cap at capacity,
client isolation, concurrent access (one shared client, and many clients created concurrently), refill boundaries
and fractional accumulation, zero refill / backwards clock, and invalid configuration.

## Benchmark

Run with: `./build/rate_limiter_benchmark` (Release build).

**Environment:** Intel Core i5-13420H (8 cores / 12 logical processors), 16 GB RAM, Windows
(`10.0.26200.0`), GCC 16.2.0 (MSYS2 UCRT64), Release build (`NDEBUG`), C++17.
**Method:** 1,000,000 `allowRequest` calls per scenario, split across threads; median of 3 runs. Limiter config:
capacity 1000, refill 1000/s per client. Buckets are created before timing, so the numbers reflect
steady-state lookups, not first-time inserts. Latency is wall time x threads / total calls, so it includes time waiting for the lock.

| Scenario | Allowed | Rejected | Time (ms) | Throughput (M req/s) | ns/call |
|---|---:|---:|---:|---:|---:|
| 1 client, 1 thread | 1,038 | 998,962 | 39.3 | 25.45 | 39.3 |
| 10k clients, 1 thread | 1,000,000 | 0 | 65.9 | 15.18 | 65.9 |
| 1 client, 2 threads | 1,074 | 998,926 | 75.1 | 13.32 | 150.1 |
| 1 client, 4 threads | 1,070 | 998,930 | 71.3 | 14.02 | 285.3 |
| 1 client, 8 threads | 1,076 | 998,924 | 77.3 | 12.94 | 618.1 |
| 10k clients, 2 threads | 1,000,000 | 0 | 154.2 | 6.49 | 308.3 |
| 10k clients, 4 threads | 1,000,000 | 0 | 156.8 | 6.38 | 627.3 |
| 10k clients, 8 threads | 1,000,000 | 0 | 162.7 | 6.15 | 1301.6 |

**What the results show**

- Throughput does **not** increase with more threads: it drops from 1 to 2 threads and then stays roughly flat.
  This is consistent with the single global mutex serializing all calls, and is the main known limitation of this design.
- Per-call latency grows roughly in proportion to the number of threads, which is also consistent with lock waiting.
- With 10k clients and a capacity of 1000, no requests are rejected, so that scenario exercises only the "allowed" path.
- These are single-machine results from a laptop and vary between runs. They were not profiled, so causes above are
  interpretations of the measurements, not proven.

## Example Output

Output from `rate_limiter_demo` (line order between clients varies from run to run; middle lines omitted):

```text
Rate limiter demo: capacity=5 tokens, refill=2 tokens/sec

Client B | Request 1 | ALLOWED
Client A | Request 1 | ALLOWED
Client C | Request 1 | ALLOWED
Client B | Request 2 | ALLOWED
...
Client B | Request 5 | ALLOWED
Client B | Request 6 | REJECTED
...
Client A | Request 6 | ALLOWED
Client C | Request 2 | ALLOWED
Client A | Request 7 | REJECTED
...

Summary
Client A: 9 allowed, 11 rejected (of 20)
Client B: 5 allowed, 15 rejected (of 20)
Client C: 8 allowed, 0 rejected (of 8)
Tracked clients: 3
```

Client B sends everything at once and is limited to its burst of 5; Client C stays under the rate and is never rejected;
neither affects the other.

## Future Improvements / Production Considerations

**None of the following is implemented.** This project is a single-process, in-memory library.

- **Lock contention:** shard the client map (N shards, each with its own mutex, chosen by hash of the client id).
  This would help many-client workloads, but a single very hot client would still serialize on one shard.
- **Memory cleanup:** evict buckets for inactive clients (for example, when a bucket is full and idle for a while).
- **Distributed rate limiting:** a shared store such as Redis, with atomic check-and-decrement (for example, via a Lua script),
  so multiple limiter instances enforce one limit. This brings network latency, consistency trade-offs, and a
  decision about behavior when the store is unavailable (fail open vs fail closed).
- **Multiple instances / sharding clients** across servers by client id.
- **Configuration management:** per-client or per-endpoint limits instead of one global setting.
- **HTTP integration:** call the limiter from a request handler and return `429 Too Many Requests`.
- **Metrics and observability:** counters for allowed/rejected requests, latency histograms.
- **Fault tolerance:** defined behavior on dependency failures.

## Tech Stack

C++17, STL (`unordered_map`, `mutex`, `atomic`, `thread`, `chrono`), CMake, multithreading.

## License

MIT, see `LICENSE`.