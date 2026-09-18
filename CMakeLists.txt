cmake_minimum_required(VERSION 3.14)
project(cpp_rate_limiter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# Core library: no dependency on HTTP, logging or any executable.
add_library(ratelimiter STATIC
    src/token_bucket.cpp
    src/rate_limiter.cpp
)
target_include_directories(ratelimiter PUBLIC include)
target_link_libraries(ratelimiter PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(ratelimiter PUBLIC /W4)
else()
    target_compile_options(ratelimiter PUBLIC -Wall -Wextra -Wpedantic)
endif()

# Concurrent demo
add_executable(rate_limiter_demo src/main.cpp)
target_link_libraries(rate_limiter_demo PRIVATE ratelimiter)

# Tests (run with: ctest --test-dir build --output-on-failure)
enable_testing()
add_executable(test_rate_limiter tests/test_rate_limiter.cpp)
target_link_libraries(test_rate_limiter PRIVATE ratelimiter)
add_test(NAME rate_limiter_tests COMMAND test_rate_limiter)

# Benchmark
add_executable(rate_limiter_benchmark benchmark/benchmark.cpp)
target_link_libraries(rate_limiter_benchmark PRIVATE ratelimiter)