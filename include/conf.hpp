#pragma once

#include <cstdint>
#include <cstddef>
#include <algorithm>

#include "essentials.hpp"
#include "util.hpp"

namespace phast {

/*
    PHast core: maps key hashes to (bucket, slice).

    Notation (from the PHast paper):
      - B = number of buckets
      - L = slice length (power of two)
      - S = number of slices = output_range + 1 - L - extra_shift

    For a key (already a 64-bit hash):
      - bucket(key)        = floor(B * key / 2^64)
      - slice_begin(key)   = floor(S * key / 2^64)
      - in_slice(key, sd)  = mult_hi(sd * MIX_C, key) & (L - 1)
      - f(key, seed)       = slice_begin(key) + in_slice(key, seed)
*/

struct generic_core {
    uint64_t buckets_num = 0;          // B
    uint16_t slice_len_minus_one = 0;  // L - 1
    uint64_t num_of_slices = 0;        // S

    generic_core() = default;

    /* Construct a core for the given output range and number of keys.
       bucket_size_100 = average bucket size * 100 (e.g. 450 for 4.5 keys/bucket).
       slice_len must be a power of two; max_shift is the chooser-specific extra shift. */
    static generic_core make(uint64_t output_range, uint64_t num_of_keys, uint16_t bucket_size_100,
                             uint16_t slice_len, uint16_t max_shift) {
        generic_core c;
        uint64_t bs = bucket_size_100;
        c.buckets_num = std::max<uint64_t>(1, (num_of_keys * 100 + bs / 2) / bs);
        c.slice_len_minus_one = slice_len - 1;
        c.num_of_slices = output_range + 1 - static_cast<uint64_t>(slice_len) -
                          static_cast<uint64_t>(max_shift);
        return c;
    }

    inline uint16_t slice_len() const { return slice_len_minus_one + 1; }

    inline uint64_t bucket_for(uint64_t key) const { return map64_to_64(key, buckets_num); }

    inline uint64_t slice_begin(uint64_t key) const { return map64_to_64(key, num_of_slices); }

    /* The largest value <= the slice_begin of any key in `bucket`. */
    inline uint64_t slice_begin_for_bucket(uint64_t bucket) const {
        return slice_begin(lowest_hash_for_bucket(bucket, buckets_num));
    }

    inline uint64_t in_slice(uint64_t key, uint16_t seed) const {
        return static_cast<uint64_t>(mix_key_seed(key, seed) & slice_len_minus_one);
    }

    inline uint64_t f(uint64_t key, uint16_t seed) const {
        return slice_begin(key) + in_slice(key, seed);
    }

    /* Output range = num_of_slices + slice_len_minus_one + extra_shift. */
    inline uint64_t output_range(uint16_t extra_shift) const {
        return num_of_slices + static_cast<uint64_t>(slice_len_minus_one) +
               static_cast<uint64_t>(extra_shift);
    }

    template <typename Visitor>
    void visit(Visitor& visitor) const {
        visit_impl(visitor, *this);
    }
    template <typename Visitor>
    void visit(Visitor& visitor) {
        visit_impl(visitor, *this);
    }

private:
    template <typename Visitor, typename T>
    static void visit_impl(Visitor& visitor, T&& t) {
        visitor.visit(t.buckets_num);
        visitor.visit(t.slice_len_minus_one);
        visitor.visit(t.num_of_slices);
    }
};

/* Returns a reasonable bucket_size_100 for the given bits_per_seed (PHast paper, table). */
constexpr uint16_t bits_per_seed_to_100_bucket_size(uint8_t bits_per_seed) {
    if (bits_per_seed <= 4) return 250;
    switch (bits_per_seed) {
        case 5: return 290;
        case 6: return 320;
        case 7: return 370;
        case 8: return 450;
        case 9: return 530;
        case 10: return 590;
        case 11: return 650;
        case 12: return 720;
        case 13: return 770;
        default: return 830;
    }
}

}  // namespace phast
