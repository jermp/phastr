#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace phast {

/* Cyclic bitset with a power-of-two number of 64-bit words.
   Bit `i` and `i + SIZE_64*64` refer to the same physical bit (mod size).

   Used by the builder to track which output values have been "claimed" by
   already-placed buckets within the sliding window. */
template <size_t SIZE_64>
struct cyclic_set {
    static_assert((SIZE_64 & (SIZE_64 - 1)) == 0, "SIZE_64 must be a power of two");
    static constexpr size_t MASK = SIZE_64 * 64 - 1;
    static constexpr size_t CHUNK_MASK = SIZE_64 - 1;

    std::array<uint64_t, SIZE_64> data{};

    cyclic_set() { data.fill(0); }

    inline void add(size_t value) {
        size_t v = value & MASK;
        data[v >> 6] |= 1ULL << (v & 63);
    }
    inline void remove(size_t value) {
        size_t v = value & MASK;
        data[v >> 6] &= ~(1ULL << (v & 63));
    }
    inline bool contain(size_t value) const {
        size_t v = value & MASK;
        return (data[v >> 6] >> (v & 63)) & 1ULL;
    }

    /* Clear the entire bit (i.e. clears the storage). */
    void clear_all() { data.fill(0); }
};

}  // namespace phast
