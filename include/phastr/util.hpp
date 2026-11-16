#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

namespace phastr {

/* Returns the most significant 64 bits of the 128-bit product of a and b. */
static inline uint64_t mul_hi(uint64_t a, uint64_t b) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * b) >> 64);
}

/* Returns the upper half of the 128-bit product of a and b xored with the
   lower half. */
static inline uint64_t folded_multiply(uint64_t a, uint64_t b) {
    __uint128_t p = static_cast<__uint128_t>(a) * b;
    return static_cast<uint64_t>(p >> 64) ^ static_cast<uint64_t>(p);
}

/* The value combined with the seed of the function to obtain the seed of the
   second hash of a key (see level_sig). */
static constexpr uint64_t SECOND_HASH = 0x5851F42D4C957F2DULL;

/* The value combined with the seed of the function to obtain the seed of the
   hash used to tell apart keys with the same signature when detecting
   duplicates. */
static constexpr uint64_t THIRD_HASH = 0xD6E8FEB86659FD93ULL;

/* The value combined with the second hash of a key to obtain the multiplier
   of level_sig. */
static constexpr uint64_t LEVEL_SIG = 0xE7037ED1A0B428DBULL;

/* Returns the signature of a key for a level after the first one, given its
   signature for the first level, a second hash of the key, and the salt of
   the level. */
static inline uint64_t level_sig(uint64_t h, uint64_t second, uint64_t salt) {
    return folded_multiply(h ^ salt, second ^ LEVEL_SIG);
}

/* Returns the salt of a level after the first one: a value depending on the
   seed of the function, on the level, and, for the last level, on the
   attempt. */
static inline uint64_t level_salt(uint64_t seed, uint64_t level, uint64_t attempt) {
    return folded_multiply(seed + level + (attempt << 32), 0x9E3779B97F4A7C15ULL);
}

/* Smallest power of two >= x (for x <= 2^63). */
static inline uint64_t next_power_of_two(uint64_t x) {
    if (x <= 1) return 1;
    return uint64_t(1) << (64 - __builtin_clzll(x - 1));
}

static inline uint32_t ilog2(uint64_t x) {
    return 63 - __builtin_clzll(x);
}

template <typename T>
static inline T ceil_div(T a, T b) {
    return (a + b - 1) / b;
}

static inline void prefetch(void const* p) {
    __builtin_prefetch(p, 0, 3);
}

/* Returns the index of the first bit set in `bits` in the range
   [begin, end), or `end` if there is none. */
static inline size_t next_set(uint64_t const* bits, size_t begin, size_t end) {
    size_t w = begin / 64;
    uint64_t word = bits[w] & (~uint64_t(0) << (begin % 64));
    while (true) {
        if (word != 0) {
            size_t j = w * 64 + __builtin_ctzll(word);
            return j < end ? j : end;
        }
        w += 1;
        if (w * 64 >= end) return end;
        word = bits[w];
    }
}

/* Runs tasks 0..num_tasks with at most num_threads threads. Each task is
   f(task_index). With one thread, or one task, runs inline. */
template <typename F>
static void parallel_for_tasks(size_t num_tasks, size_t num_threads, F const& f) {
    if (num_tasks == 0) return;
    size_t threads = std::min(num_tasks, std::max<size_t>(num_threads, 1));
    if (threads == 1) {
        for (size_t t = 0; t != num_tasks; ++t) f(t);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (size_t i = 0; i != threads; ++i) {
        pool.emplace_back([&]() {
            while (true) {
                size_t t = next.fetch_add(1, std::memory_order_relaxed);
                if (t >= num_tasks) break;
                f(t);
            }
        });
    }
    for (auto& t : pool) t.join();
}

}  // namespace phastr
