#pragma once

#include <cstdint>
#include <cstddef>

namespace phast {

/* Returns most significant 64-bit half of a*b. */
static inline uint64_t mult_hi(uint64_t a, uint64_t b) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b)) >> 64);
}

/* Returns floor((key * n) / 2^64) - i.e. maps key in [0, 2^64) to [0, n). */
static inline uint64_t map64_to_64(uint64_t key, uint64_t n) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(key) * static_cast<__uint128_t>(n)) >>
                                 64);
}

/* mix(a,b) = (high64(a*b)) XOR (low64(a*b)) */
static inline uint64_t mix64(uint64_t a, uint64_t b) {
    __uint128_t r = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
    return static_cast<uint64_t>(r >> 64) ^ static_cast<uint64_t>(r);
}

/* Mixing constant from FxHash, also used in PTHash. */
static constexpr uint64_t MIX_C = 0x517cc1b727220a95ULL;

/* Mixing constant used by PHast's `in_slice_nobump` path. */
static constexpr uint64_t MIX_NB = 0xa0761d6478bd642fULL;
static constexpr uint64_t MIX_NB2 = 0x1d8e4e27c47d124fULL;

/* Returns mix_key_seed(key, seed) used by PHast: high bits of (seed * MIX_C) * key. */
static inline uint16_t mix_key_seed(uint64_t key, uint16_t seed) {
    return static_cast<uint16_t>(mult_hi(static_cast<uint64_t>(seed) * MIX_C, key));
}

/* Smallest power of two >= x (for x > 0, x <= 2^63). */
static inline uint64_t next_power_of_two(uint64_t x) {
    if (x <= 1) return 1;
    return 1ULL << (64 - __builtin_clzll(x - 1));
}

/* Ceiling of a/b for unsigned integers. */
template <typename T>
static inline T ceil_div(T a, T b) {
    return (a + b - 1) / b;
}

/* The lowest hash code that maps to bucket b under map64_to_64(., buckets_num).
   That is, the smallest c such that floor(buckets_num * c / 2^64) == b,
   which equals ceil(b * 2^64 / buckets_num). */
static inline uint64_t lowest_hash_for_bucket(uint64_t bucket, uint64_t buckets_num) {
    if (buckets_num == 0) return 0;
    __uint128_t numerator = static_cast<__uint128_t>(bucket) << 64;
    __uint128_t q = numerator / buckets_num;
    if (q * buckets_num != numerator) q += 1;  // ceiling division
    return static_cast<uint64_t>(q);
}

}  // namespace phast
