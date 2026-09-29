#pragma once

// Lowest-overhead cycle/tick counter available on the platform, for timing
// individual engine calls.
//
//   x86-64       : RDTSC with LFENCE ordering (invariant TSC, ~0.3 ns ticks),
//                  converted to ns by calibrating against steady_clock.
//   aarch64 Linux: CNTVCT_EL0 generic timer (frequency from CNTFRQ_EL0).
//   otherwise    : std::chrono::steady_clock. On Apple Silicon macOS this
//                  ticks at 24 MHz, i.e. 41.67 ns resolution.

#include <chrono>
#include <cstdint>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace lob::timer {

#if defined(__x86_64__)

inline std::uint64_t now() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
inline const char* name() { return "rdtsc"; }

#elif defined(__aarch64__) && defined(__linux__)

inline std::uint64_t now() noexcept {
    std::uint64_t t;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(t)::"memory");
    return t;
}
inline const char* name() { return "cntvct_el0"; }

#else

inline std::uint64_t now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}
inline const char* name() { return "steady_clock"; }

#endif

// Nanoseconds per tick, measured once against steady_clock over ~200 ms.
inline double ns_per_tick() {
    static const double value = [] {
        using C = std::chrono::steady_clock;
        const auto c0 = C::now();
        const std::uint64_t t0 = now();
        while (C::now() - c0 < std::chrono::milliseconds(200)) {
        }
        const auto c1 = C::now();
        const std::uint64_t t1 = now();
        const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
        return ns / static_cast<double>(t1 - t0);
    }();
    return value;
}

}  // namespace lob::timer
