#pragma once
#include <cstdint>
#include <chrono>

inline uint64_t os_gettime_ns(void) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
