#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "util.hpp"

namespace phast {

/*
    A "seeded" hasher that takes a key and a seed (interpreted as the level number)
    and produces a uint64_t hash.

    The default hasher works for `uint64_t` keys (which is the most common PHast use case)
    by mixing the key with the seed using a strong 64-bit mixer.
    Specializations are provided for byte ranges and std::string via a simple
    polynomial-style hash. For other key types, the user can supply their own hasher.
*/

/* SplitMix64 with a seed used as bias. */
static inline uint64_t splitmix64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

struct default_hasher {
    /* For uint64_t keys: combine the key with the seed via a strong mix. */
    static inline uint64_t hash_one(uint64_t key, uint64_t seed) {
        return splitmix64(key + splitmix64(seed));
    }

    /* For byte ranges. */
    static inline uint64_t hash_one(uint8_t const* data, size_t n, uint64_t seed) {
        uint64_t h = splitmix64(seed) ^ 0x9e3779b97f4a7c15ULL;
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t w;
            __builtin_memcpy(&w, data + i, 8);
            h ^= w;
            h = h * 0x100000001b3ULL;
        }
        uint64_t tail = 0;
        size_t r = n - i;
        if (r > 0) {
            __builtin_memcpy(&tail, data + i, r);
            h ^= tail;
            h = h * 0x100000001b3ULL;
        }
        h ^= static_cast<uint64_t>(n);
        return splitmix64(h);
    }

    static inline uint64_t hash_one(std::string const& s, uint64_t seed) {
        return hash_one(reinterpret_cast<uint8_t const*>(s.data()), s.size(), seed);
    }
};

}  // namespace phast
