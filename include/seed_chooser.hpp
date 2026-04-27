#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <vector>

#include "conf.hpp"
#include "cyclic.hpp"
#include "evaluator.hpp"

namespace phast {

/*
    Default `SeedOnly` chooser: regular PHast.

    For each bucket the chooser searches seed s = 1, 2, ..., (1<<bits_per_seed) - 1
    looking for the seed that minimizes the sum of f(key, s) over the bucket's keys
    while not colliding with already-used values nor self-colliding (two keys mapping
    to the same value with that seed).

    Seed `0` is special and means the bucket is bumped to the next level.

    UsedValues is a cyclic bitset over a window of MAX_VALUES = 16384 positions
    (large enough for slice_len up to 8192).
*/

constexpr size_t MAX_VALUES = 4096 * 2 * 2;  // 16384
constexpr size_t MAX_WINDOW_SIZE = 256;
constexpr uint16_t WINDOW_SIZE = 256;
constexpr size_t SMALL_BUCKET_LIMIT = 8;

using used_value_set = cyclic_set<MAX_VALUES / 64>;

struct seed_only {
    static constexpr bool BUMPING = true;
    static constexpr uint16_t FIRST_SEED = 1;

    using used_values = used_value_set;

    static inline uint16_t extra_shift(uint8_t /*bits_per_seed*/) { return 0; }

    static inline uint64_t f(uint64_t key, uint16_t seed, generic_core const& c) {
        return c.f(key, seed);
    }

    static inline uint64_t minimal_output_range(uint64_t num_of_keys) { return num_of_keys; }

    static inline weights bucket_evaluator(uint8_t bits_per_seed, uint16_t slice_len) {
        return weights::make(bits_per_seed, slice_len);
    }

    /* Slice length for regular PHast: a function of the output range and bits_per_seed. */
    static inline uint16_t slice_len(uint64_t output_without_shift, uint8_t bits_per_seed,
                                     uint16_t preferred_slice_len) {
        if (output_without_shift < 64) {
            uint64_t n = output_without_shift;
            uint64_t v = (n / 2) + 1;
            uint64_t pw = 1;
            while (pw < v) pw <<= 1;
            return static_cast<uint16_t>(pw);
        }
        if (output_without_shift < 1300) return 64;
        if (output_without_shift < 9500) return 128;
        if (output_without_shift < 12000) return 256;
        if (output_without_shift < 140000) return 512;
        if (bits_per_seed < 6) return preferred_slice_len ? preferred_slice_len : 512;
        if (bits_per_seed < 12) return preferred_slice_len ? preferred_slice_len : 1024;
        return preferred_slice_len ? preferred_slice_len : 2048;
    }

    /* Searches for the best seed for a single bucket. */
    template <typename Core>
    static uint16_t best_seed_search(used_values& uv, uint64_t const* keys, size_t keys_len,
                                     Core const& core, uint8_t bits_per_seed) {
        uint16_t seeds_num = static_cast<uint16_t>(1u << bits_per_seed);
        uint16_t best_seed = 0;
        uint64_t best_value = std::numeric_limits<uint64_t>::max();
        std::array<uint64_t, SMALL_BUCKET_LIMIT> stack_buf{};
        std::vector<uint64_t> heap_buf;
        uint64_t* values = stack_buf.data();
        bool small = keys_len <= SMALL_BUCKET_LIMIT;
        if (!small) {
            heap_buf.resize(keys_len);
            values = heap_buf.data();
        }
        for (uint16_t s = FIRST_SEED; s < seeds_num; ++s) {
            bool collide = false;
            for (size_t i = 0; i < keys_len; ++i) {
                uint64_t v = core.f(keys[i], s);
                if (uv.contain(v)) {
                    collide = true;
                    break;
                }
                values[i] = v;
            }
            if (collide) continue;
            uint64_t sum = 0;
            for (size_t i = 0; i < keys_len; ++i) sum += values[i];
            if (sum < best_value) {
                std::sort(values, values + keys_len);
                bool self_collision = false;
                for (size_t i = 1; i < keys_len; ++i) {
                    if (values[i - 1] == values[i]) {
                        self_collision = true;
                        break;
                    }
                }
                if (self_collision) continue;
                best_value = sum;
                best_seed = s;
            }
        }
        if (best_seed != 0) {
            for (size_t i = 0; i < keys_len; ++i) uv.add(core.f(keys[i], best_seed));
        }
        return best_seed;
    }
};

}  // namespace phast
