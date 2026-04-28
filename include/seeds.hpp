#pragma once

#include <cstdint>
#include <vector>
#include <stdexcept>

#include "essentials.hpp"

namespace phast {

/*
    SeedSize abstraction.

    Each "bucket" in PHast stores a small integer "seed" (0..2^bits_per_seed - 1).
    Seeds are packed into a backing vector of `vec_element` words. This struct
    abstracts how seeds are stored and accessed.

    The default `bits8_seed_size` stores one seed per byte (uint8_t). It is the
    fastest variant and corresponds to PHast's `Bits8` Rust type. Bits-per-seed
    is fixed at compile time to 8.

    A more flexible `bits_seed_size` stores arbitrary bit-widths in 64-bit words.
*/

/* 8-bit-per-seed implementation - the most common variant. */
struct bits8_seed_size {
    using vec_element = uint8_t;
    static constexpr uint8_t bits_per_seed = 8;
    static constexpr size_t vec_element_bit_size = 8;

    bits8_seed_size() = default;

    static size_t num_elements(size_t num_seeds) {
        return num_seeds;
    }

    static std::vector<vec_element> new_zeroed(size_t num_seeds) {
        return std::vector<vec_element>(num_seeds, 0);
    }

    static inline uint16_t get(vec_element const* vec, size_t index) {
        return static_cast<uint16_t>(vec[index]);
    }
    static inline void set(vec_element* vec, size_t index, uint16_t seed) {
        vec[index] = static_cast<uint8_t>(seed);
    }
};

/* Variable-bit seed implementation, packed into 64-bit words. */
struct bits_seed_size {
    using vec_element = uint64_t;
    static constexpr size_t vec_element_bit_size = 64;

    uint8_t bits_per_seed = 8;

    bits_seed_size() = default;
    explicit bits_seed_size(uint8_t bps) : bits_per_seed(bps) {
        if (bps == 0 || bps > 16) throw std::invalid_argument("bits_per_seed must be in [1, 16]");
    }

    size_t num_elements(size_t num_seeds) const {
        size_t total_bits = num_seeds * static_cast<size_t>(bits_per_seed);
        return (total_bits + 63) / 64;
    }

    std::vector<vec_element> new_zeroed(size_t num_seeds) const {
        return std::vector<vec_element>(num_elements(num_seeds), 0);
    }

    inline uint64_t mask() const {
        return (1ULL << bits_per_seed) - 1;
    }

    inline uint16_t get(vec_element const* vec, size_t index) const {
        size_t bit = index * bits_per_seed;
        size_t w = bit >> 6;
        size_t s = bit & 63;
        uint64_t m = mask();
        if (s + bits_per_seed <= 64) return static_cast<uint16_t>((vec[w] >> s) & m);
        uint64_t lo = vec[w] >> s;
        uint64_t hi = vec[w + 1] << (64 - s);
        return static_cast<uint16_t>((lo | hi) & m);
    }
    inline void set(vec_element* vec, size_t index, uint16_t seed) const {
        uint64_t v = static_cast<uint64_t>(seed) & mask();
        size_t bit = index * bits_per_seed;
        size_t w = bit >> 6;
        size_t s = bit & 63;
        uint64_t m = mask();
        vec[w] &= ~(m << s);
        vec[w] |= v << s;
        if (s + bits_per_seed > 64) {
            vec[w + 1] &= ~(m >> (64 - s));
            vec[w + 1] |= v >> (64 - s);
        }
    }
};

}  // namespace phast
