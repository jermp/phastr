#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>

#include "util.hpp"

namespace phastr {

/* Number of buckets in the window of a sweep. */
static constexpr size_t WINDOW = 256;

/* The distance of the "origin" of a bucket from the beginning of the
   leftmost slice that a key of the bucket can have (see sweep::cost). */
static constexpr size_t ORIGIN_OFFSET = 95;

/* The number of fractional bits of the base-2 logarithms used by
   sweep::cost. */
static constexpr uint32_t LOG2_FRACTION = 24;

/* The maximum number of keys of the last level, which is built without
   bumping. */
static constexpr size_t LAST_LEVEL_THRESHOLD = 4096;

/* The maximum number of attempts at building the last level. */
static constexpr uint64_t LAST_LEVEL_ATTEMPTS = 1000;

/* Default size-dependent priority weights, tuned for PHastR with four
   layouts (see the PHastR paper): the first set for seeds of at most eight
   bits, the second for longer seeds. */
static inline std::array<int64_t, 7> default_weights(uint32_t seed_bits) {
    if (seed_bits <= 8) return {-52000, 53315, 105861, 138629, 156944, 175176, 186591};
    return {46000, 55041, 137806, 184081, 217321, 241846, 256427};
}

/* The parameters of the construction of a phastr::function. */
struct build_configuration {
    /* The number of bits of a bucket seed, in [2..16]; at most 8 with the
       default byte seed storage. */
    uint32_t seed_bits = 8;
    /* The base-2 logarithm of the number of layouts, at most 6; the number
       of layouts times log2_slice_len must be at most 64. */
    uint32_t log2_layouts = 2;
    /* The base-2 logarithm of the (maximum) slice length, in [3..16]. */
    uint32_t log2_slice_len = 10;
    /* The expected number of keys of a bucket (lambda). */
    double bucket_size = 4.25;
    /* The seed of the hash function. */
    uint64_t seed = 0;
    /* The number of threads used by the construction. */
    uint64_t num_threads = 1;
    /* The maximum number of chunks in which a level is swept in parallel;
       0 means num_threads. The function built depends on this value. */
    uint64_t sweep_chunks = 0;
    /* Keep the records of the keys in temporary files rather than in
       memory (external-memory construction). The structure built is
       identical. */
    bool external_memory = false;
    /* The directory of the temporary files of the external-memory
       construction. */
    std::string tmp_dir = "";
    /* Custom priority weights (used if use_weights is true). */
    bool use_weights = false;
    std::array<int64_t, 7> weights = {};
    bool verbose = false;

    std::array<int64_t, 7> priority_weights() const {
        return use_weights ? weights : default_weights(seed_bits);
    }

    uint64_t max_sweep_chunks() const {
        return sweep_chunks != 0 ? sweep_chunks : std::max<uint64_t>(num_threads, 1);
    }

    void check() const {
        if (seed_bits < 2 || seed_bits > 16) {
            throw std::runtime_error("phastr: the number of seed bits must be in [2..16]");
        }
        if (log2_layouts >= seed_bits) {
            throw std::runtime_error("phastr: too many layouts for the number of seed bits");
        }
        if (log2_slice_len < 3 || log2_slice_len > 16) {
            throw std::runtime_error(
                "phastr: the base-2 logarithm of the slice length must be in [3..16]");
        }
        if (log2_layouts > 6 || (log2_slice_len << log2_layouts) > 64) {
            throw std::runtime_error("phastr: too many layouts for the slice length");
        }
        const double max_bucket_size =
            std::min<double>((uint64_t(1) << log2_slice_len) / 8.0, 64.0);
        if (bucket_size < 1.0 || bucket_size > max_bucket_size) {
            throw std::runtime_error("phastr: the expected bucket size must be in [1.." +
                                     std::to_string(max_bucket_size) +
                                     "] for the given slice length");
        }
        if (use_weights) {
            for (auto w : weights) {
                if (w > (int64_t(1) << 32) || w < -(int64_t(1) << 32)) {
                    throw std::runtime_error("phastr: the weights must be in [-2^32..2^32]");
                }
            }
        }
    }
};

/* The parameters of a level of the structure (query side). */
struct level_params {
    /* The number of buckets (always odd). */
    uint64_t buckets = 0;
    /* The number of possible slice starts (the output range minus the slice
       length, plus one). */
    uint64_t num_slices = 0;
    /* The slice length minus one. */
    uint64_t l_mask = 0;
    /* The base-2 logarithm of the amount u by which a unit increase of the
       seed moves the keys of a bucket. */
    uint64_t scale = 0;
    /* The offset of the outputs of this level in the remapping sequence. */
    uint64_t offset = 0;
    /* The salt used to compute the signatures of the keys of this level
       from their hashes (unused for the first level). */
    uint64_t salt = 0;
    /* The index of the first seed of this level in the seed storage. */
    uint64_t first_seed = 0;

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
        visitor.visit(t.buckets);
        visitor.visit(t.num_slices);
        visitor.visit(t.l_mask);
        visitor.visit(t.scale);
        visitor.visit(t.offset);
        visitor.visit(t.salt);
        visitor.visit(t.first_seed);
    }
};

/* The geometry of a level during construction. */
struct geometry {
    /* The number of buckets (always odd). */
    uint64_t buckets = 1;
    /* The number of possible slice starts divided by the number of buckets,
       in 64.64 fixed point (rounded down). */
    __uint128_t slices_per_bucket = 0;
    /* The number of possible slice starts. */
    uint64_t num_slices = 1;
    /* The slice length minus one. */
    uint64_t l_mask = 0;
    /* The base-2 logarithm of the amount u by which a unit increase of the
       seed moves the keys of a bucket. */
    uint32_t scale = 0;
    /* The base-2 logarithm of the number of layouts. */
    uint32_t log2_layouts = 2;
    /* The number of bits of a seed. */
    uint32_t seed_bits = 8;
    /* The output range. */
    uint64_t m = 1;
    /* The maximum number of chunks of a parallel sweep. */
    uint64_t max_chunks = 1;

    /* Computes the geometry of a level with k keys and output range m. */
    static geometry make(build_configuration const& config, uint64_t k, uint64_t m,
                         double bucket_size) {
        geometry g;
        m = std::max<uint64_t>(m, 1);
        /* The slices of small levels span about half of the output range. */
        const uint64_t l =
            std::min<uint64_t>(next_power_of_two(m / 2 + 1), uint64_t(1) << config.log2_slice_len);
        /* The number of buckets must be odd, as offsets are taken from the
           product of the signature and the number of buckets. */
        uint64_t nb = static_cast<uint64_t>(
            std::max<double>(std::nearbyint(static_cast<double>(k) / bucket_size), 1.0));
        g.buckets = nb | 1;
        g.num_slices = m + 1 - l;
        g.slices_per_bucket = (static_cast<__uint128_t>(g.num_slices) << 64) / g.buckets;
        g.l_mask = l - 1;
        /* With slices having fewer slots than there are seeds, the seed is
           not shifted, and rotations are redundant. */
        const uint32_t log2_l = ilog2(l);
        g.scale = log2_l > config.seed_bits ? log2_l - config.seed_bits : 0;
        g.log2_layouts = config.log2_layouts;
        g.seed_bits = config.seed_bits;
        g.m = m;
        g.max_chunks = config.max_sweep_chunks();
        return g;
    }

    /* Returns the bucket of a key with the given signature. */
    inline uint64_t bucket(uint64_t h) const {
        return mul_hi(h, buckets);
    }

    /* Returns the beginning of the slice of a key with the given
       signature. */
    inline uint64_t slice_begin(uint64_t h) const {
        return mul_hi(h, num_slices);
    }

    /* Returns a lower bound for the beginning of the slices of the keys of
       bucket b and of the following buckets. */
    inline uint64_t first_slice(uint64_t b) const {
        const uint64_t hi = static_cast<uint64_t>(slices_per_bucket >> 64);
        const uint64_t lo = static_cast<uint64_t>(slices_per_bucket);
        return b * hi + mul_hi(b, lo);
    }

    /* Returns the maximum distance between the first slice of a bucket and
       the beginning of the slice of a key of the bucket. */
    inline uint64_t max_slice_distance() const {
        return ceil_div(num_slices, buckets) + 1;
    }

    /* Returns the number of buckets between chunks of a parallel sweep
       whose slots cannot overlap. */
    inline uint64_t chunk_gap() const {
        return static_cast<uint64_t>(static_cast<__uint128_t>(l_mask + 1) * buckets / num_slices) +
               1;
    }

    /* Returns the number of chunks of a parallel sweep: at most max_chunks,
       and such that each chunk has at least 4096 buckets and 64 times the
       gap between chunks. */
    inline uint64_t num_sweep_chunks() const {
        const uint64_t max_by_size = buckets / std::max<uint64_t>(64 * chunk_gap(), 4096);
        return std::max<uint64_t>(std::min<uint64_t>(max_chunks, max_by_size), 1);
    }

    /* Returns the value providing the in-slice offsets of the layouts: the
       lower half of the product of the signature and the number of buckets,
       whose upper half is the bucket. */
    inline uint64_t offsets(uint64_t h) const {
        return h * buckets;
    }

    /* Returns the slot of a key with the given signature when its bucket
       has the given seed. */
    inline uint64_t pos(uint64_t h, uint64_t seed) const {
        const uint64_t r = seed & ((uint64_t(1) << log2_layouts) - 1);
        const uint64_t offset = (offsets(h) >> (r * (64 >> log2_layouts))) + (seed << scale);
        return slice_begin(h) + (offset & l_mask);
    }

    /* Returns the parameters of a level with this geometry; the offset in
       the remapping sequence, the salt, and the index of the first seed are
       zero. */
    level_params level() const {
        level_params p;
        p.buckets = buckets;
        p.num_slices = num_slices;
        p.l_mask = l_mask;
        p.scale = scale;
        return p;
    }
};

}  // namespace phastr
