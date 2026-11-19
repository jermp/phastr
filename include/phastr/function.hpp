#pragma once

/*
    PHastR: a bucket-placement minimal perfect hash function with rotated
    layouts, by Sebastiano Vigna:

        Sebastiano Vigna. "PHastR: Faster Minimal Perfect Hashing with
        Rotated Layouts", 2026. https://arxiv.org/abs/2610.17833

    Each key is hashed to a 64-bit signature, which determines its bucket
    and its slice, a window of consecutive slots of the output range. The
    seed of a bucket selects one of the layouts (by default, four), which
    give each key an offset in its slice, and a rotation of the offsets;
    seeds are chosen so that no two keys have the same slot. Seed zero marks
    the buckets that could not be placed, whose keys are bumped to the
    following level, a smaller function whose slots are remapped to the free
    slots of the first level with an Elias-Fano sequence.

    With the default parameters (8-bit seeds, four layouts, slices of length
    1024, and an expected bucket size of 4.25 keys) space is about 1.94 bits
    per key.

    Keys are read once by the build methods, which store a 16-byte record
    for each key: in memory (build_in_internal_memory) or in temporary files
    (build_in_external_memory). The two methods build identical structures,
    which do not depend on the number of threads (they depend on the maximum
    number of sweep chunks, which defaults to the number of threads).
*/

#include <cstddef>
#include <iterator>
#include <string>
#include <type_traits>

#include "essentials.hpp"
#include "elias_fano.hpp"

#include "util.hpp"
#include "geometry.hpp"
#include "builder.hpp"
#include "seed_store.hpp"

namespace phastr {

/* SplitMix64. */
static inline uint64_t splitmix64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

/* A seeded hasher producing a 64-bit signature: it is constructed from a
   64-bit seed, which it preprocesses once, so that queries pay only the
   per-key work (without this, every query would pay a dependent chain to
   rehash the seed, even at call sites that cannot hoist it). The default
   works for 64-bit integer keys; for other key types, supply your own
   hasher with the same interface: a constructor from the seed, a hashing
   operator(), and a visit method serializing any preprocessed state. */
struct default_hasher {
    uint64_t prepared;

    default_hasher() : prepared(splitmix64(0)) {}
    explicit default_hasher(uint64_t seed) : prepared(splitmix64(seed)) {}

    __attribute__((always_inline)) inline uint64_t operator()(uint64_t key) const {
        return splitmix64(key + prepared);
    }

    template <typename Visitor>
    void visit(Visitor& visitor) const {
        visitor.visit(prepared);
    }
    template <typename Visitor>
    void visit(Visitor& visitor) {
        visitor.visit(prepared);
    }
};

/* The base-2 logarithm of the distance in bits between the offsets of two
   consecutive layouts with the default number of layouts (four). */
static constexpr uint32_t DEFAULT_LAYOUT_SHIFT = 4;

/* The scale of large levels with the default parameters (8-bit seeds and
   slices of length 1024). */
static constexpr uint32_t DEFAULT_SCALE = 2;

/* Returns, in its lowest 16 bits, the field of `lo` of layout r = s mod 4
   with four layouts (the default). The result agrees in its lowest 16 bits
   with that of shifting `lo` right by 16s (as shifts are taken modulo 64).

   On x86-64, reading the field from memory (the load is satisfied by
   store-to-load forwarding) avoids a variable shift whose count depends on
   a cache miss, which reduces memory-level parallelism (see the PHastR
   paper). On other architectures, notably Apple Silicon, forwarding a
   two-byte load at a variable offset from an eight-byte store stalls the
   dependent chain, and the shift is faster. Define
   PHASTR_LAYOUT_FIELD_LOAD or PHASTR_LAYOUT_FIELD_SHIFT to override the
   choice. */
__attribute__((always_inline)) static inline uint64_t default_layout_field(uint64_t lo,
                                                                           uint64_t s) {
#if !defined(PHASTR_LAYOUT_FIELD_SHIFT) &&                                                   \
    (defined(PHASTR_LAYOUT_FIELD_LOAD) || (defined(__x86_64__) && defined(__BYTE_ORDER__) && \
                                           __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
    uint16_t field;
    std::memcpy(&field, reinterpret_cast<char const*>(&lo) + 2 * (s & 3), 2);
    return field;
#else
    return lo >> ((s << DEFAULT_LAYOUT_SHIFT) & 63);
#endif
}

namespace detail {

/* Whether an iterator is random-access; false also for hand-rolled
   iterators without iterator_traits, which are read sequentially. */
template <typename Iterator, typename = void>
struct is_random_access_iterator : std::false_type {};
template <typename Iterator>
struct is_random_access_iterator<
    Iterator, std::void_t<typename std::iterator_traits<Iterator>::iterator_category>>
    : std::is_base_of<std::random_access_iterator_tag,
                      typename std::iterator_traits<Iterator>::iterator_category> {};

}  // namespace detail

/* The minimum number of keys of each writer of a parallel computation of
   the records. */
static constexpr uint64_t MIN_KEYS_PER_HASH_WRITER = 1 << 16;

template <typename Hasher = default_hasher, typename SeedStore = byte_seeds>
struct function {
    /* PHastR is always a minimal perfect hash function. */
    static constexpr bool minimal = true;

    function() = default;

    /* Builds the function on n keys read (once) from a forward iterator,
       keeping the records of the keys in memory. */
    template <typename Iterator>
    void build_in_internal_memory(Iterator keys, uint64_t n, build_configuration const& config) {
        build_configuration c = config;
        c.external_memory = false;
        build(keys, n, c);
    }

    /* Builds the function on n keys read (once) from a forward iterator,
       keeping the records of the keys in temporary files: on large key
       sets, construction needs in memory little more than the structure
       being built. The structure is identical to that built in internal
       memory. */
    template <typename Iterator>
    void build_in_external_memory(Iterator keys, uint64_t n, build_configuration const& config) {
        build_configuration c = config;
        c.external_memory = true;
        build(keys, n, c);
    }

    template <typename Iterator>
    void build(Iterator keys, uint64_t n, build_configuration const& config) {
        config.check();
        if (config.seed_bits > SeedStore::max_bits) {
            throw std::runtime_error(
                "phastr: too many seed bits for this seed storage (use compact_seeds)");
        }
        /* Compute and store the records of the keys. The number of
           partitions does not affect the structure built: in memory we use
           fewer partitions, so that a writer writes to fewer places at
           the same time.

           With a random-access iterator, the records are computed by
           num_threads writers over disjoint ranges of the keys; the
           structure built does not depend on the division into ranges (the
           seed of a bucket depends only on the multiset of the signatures
           of its keys), and the writers share the memory of one: in
           memory, each run is reserved for its expected share of the
           records; on disk, the blocks of each writer are smaller by the
           number of writers. A forward iterator is read sequentially. */
        const uint32_t log2_partitions =
            config.external_memory ? MAX_LOG2_PARTITIONS : log2_partitions_for(n);
        uint64_t hash_writers = 1;
        if constexpr (detail::is_random_access_iterator<Iterator>::value) {
            /* Each writer gets at least MIN_KEYS_PER_HASH_WRITER keys. */
            hash_writers = std::min<uint64_t>(std::max<uint64_t>(config.num_threads, 1),
                                              std::max<uint64_t>(n / MIN_KEYS_PER_HASH_WRITER, 1));
        }
        store_writer writer(sig_fn::level0(), log2_partitions, config.external_memory,
                            config.tmp_dir, n, hash_writers);
        m_hasher = Hasher(config.seed);
        m_second_hasher = Hasher(config.seed ^ SECOND_HASH);
        if (hash_writers == 1) {
            store_buffers buffers(writer, n);
            Iterator it = keys;
            for (uint64_t i = 0; i != n; ++i, ++it) {
                auto const& key = *it;
                record r;
                r.h = m_hasher(key);
                r.second = m_second_hasher(key);
                buffers.push_first_level(r);
            }
            buffers.finish();
        } else {
            if constexpr (detail::is_random_access_iterator<Iterator>::value) {
                parallel_for_tasks(hash_writers, hash_writers, [&](size_t t) {
                    const uint64_t begin = t * n / hash_writers;
                    const uint64_t end = (t + 1) * n / hash_writers;
                    store_buffers buffers(writer, end - begin);
                    Iterator it = keys + static_cast<std::ptrdiff_t>(begin);
                    for (uint64_t i = begin; i != end; ++i, ++it) {
                        auto const& key = *it;
                        record r;
                        r.h = m_hasher(key);
                        r.second = m_second_hasher(key);
                        buffers.push_first_level(r);
                    }
                    buffers.finish();
                });
            }
        }
        store st = writer.finish(n);
        build_result res = config.seed_bits <= 8
                               ? detail::build_levels<uint8_t>(config, std::move(st), n)
                               : detail::build_levels<uint16_t>(config, std::move(st), n);
        assemble(config, std::move(res));
    }

    /* Returns the value associated with the given key, in [0, num_keys()):
       different keys of the original set are mapped to different values. If
       the key was not in the original set, the result is arbitrary. */
    template <typename Key>
    __attribute__((always_inline)) inline uint64_t operator()(Key const& key) const {
        const uint64_t h = m_hasher(key);
        const __uint128_t p = static_cast<__uint128_t>(h) * m_params0.buckets;
        const uint64_t s = m_seeds0.get(static_cast<uint64_t>(p >> 64));
        if (s != 0) return pos0(h, static_cast<uint64_t>(p), s);
        return get_slow(h, m_second_hasher(key));
    }

    /* Returns, in out[0..B), the values associated with the keys
       keys[0..B), hashing all the keys and prefetching the seeds of their
       buckets before computing any value: with independent lookups in
       flight, the memory accesses of the batch overlap, so on large
       functions a batched stream of lookups is faster than one lookup at
       a time (batches of 16 to 64 keys work well). */
    template <uint64_t B, typename Key>
    inline void lookup_batch(Key const* keys, uint64_t* out) const {
        uint64_t hashes[B];
        for (uint64_t i = 0; i != B; ++i) {
            const uint64_t h = m_hasher(keys[i]);
            m_seeds0.prefetch_seed(mul_hi(h, m_params0.buckets));
            hashes[i] = h;
        }
        for (uint64_t i = 0; i != B; ++i) {
            const uint64_t h = hashes[i];
            const __uint128_t p = static_cast<__uint128_t>(h) * m_params0.buckets;
            const uint64_t s = m_seeds0.get(static_cast<uint64_t>(p >> 64));
            out[i] = s != 0 ? pos0(h, static_cast<uint64_t>(p), s)
                            : get_slow(h, m_second_hasher(keys[i]));
        }
    }

    /* Returns whether the given key is bumped from the first level. */
    template <typename Key>
    bool is_bumped(Key const& key) const {
        const uint64_t h = m_hasher(key);
        return m_seeds0.get(mul_hi(h, m_params0.buckets)) == 0;
    }

    uint64_t num_keys() const {
        return m_num_keys;
    }

    uint64_t table_size() const {
        return m_num_keys;
    }

    uint64_t seed() const {
        return m_seed;
    }

    uint64_t num_levels() const {
        return m_params.size() + 1;
    }

    uint64_t num_bits() const {
        return 8 * (sizeof(m_seed) + sizeof(m_num_keys) + sizeof(m_layout_shift) +
                    sizeof(m_default_shifts) + sizeof(m_fast_scale) + sizeof(level_params) +
                    m_params.size() * sizeof(level_params) + m_remap.num_bytes()) +
               m_seeds0.num_bits() + m_seeds.num_bits();
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
    /* The seed used to compute signatures. */
    uint64_t m_seed = 0;
    /* The hashers of the signatures and of the second hashes, prepared
       from the seed once, at construction. */
    Hasher m_hasher;
    Hasher m_second_hasher;
    /* The number of keys. */
    uint64_t m_num_keys = 0;
    /* The base-2 logarithm of the distance in bits between the offsets of
       two consecutive layouts (i.e., six minus the base-2 logarithm of the
       number of layouts). */
    uint32_t m_layout_shift = DEFAULT_LAYOUT_SHIFT;
    /* Whether the shifts of the first level are those of the default
       parameters, in which case queries use constants. */
    uint8_t m_default_shifts = 0;
    /* The scale of the first level plus one, if there are four layouts and
       the scale is at most three, in which case queries use constants even
       if the shifts are not the default ones; zero otherwise. */
    uint8_t m_fast_scale = 0;
    /* The parameters of the first level. */
    level_params m_params0;
    /* The seeds of the first level. */
    SeedStore m_seeds0;
    /* The parameters of the following levels. */
    std::vector<level_params> m_params;
    /* The seeds of the following levels, concatenated. */
    SeedStore m_seeds;
    /* The remapping, which maps the slots of the following levels to the
       holes of the first one. */
    bits::elias_fano<false, false> m_remap;

    void assemble(build_configuration const& config, build_result&& res) {
        m_seed = config.seed;
        m_num_keys = res.num_keys;
        m_layout_shift = 6 - config.log2_layouts;
        const bool default_layouts = m_layout_shift == DEFAULT_LAYOUT_SHIFT;
        m_default_shifts = default_layouts && res.params0.scale == DEFAULT_SCALE;
        m_fast_scale = default_layouts && res.params0.scale <= 3
                           ? static_cast<uint8_t>(res.params0.scale + 1)
                           : 0;
        m_params0 = res.params0;
        if (!res.seeds0_bytes.empty()) {
            m_seeds0.build(std::move(res.seeds0_bytes), config.seed_bits);
        } else {
            m_seeds0.build(res.seeds0_wide, config.seed_bits);
        }
        m_params = std::move(res.params);
        m_seeds.build(res.seeds, config.seed_bits);
        if (!res.remap_values.empty()) {
            m_remap.encode(res.remap_values.begin(), res.remap_values.size(),
                           res.remap_values.back());
        }
    }

    /* Returns the output of a key in the given level, given its signature h
       for the level, the lower half `lo` of the product of h and the number
       of buckets, the seed of the bucket, the base-2 logarithm of the width
       of the field of each layout in `lo`, and the scale of the level. */
    __attribute__((always_inline)) static inline uint64_t pos(level_params const& lv, uint64_t h,
                                                              uint64_t lo, uint64_t s,
                                                              uint32_t layout_shift,
                                                              uint32_t scale) {
        const uint64_t offset = (lo >> ((s << layout_shift) & 63)) + (s << scale);
        return mul_hi(h, lv.num_slices) + (offset & lv.l_mask);
    }

    __attribute__((always_inline)) static inline uint64_t pos_default(level_params const& lv,
                                                                      uint64_t h, uint64_t lo,
                                                                      uint64_t s) {
        const uint64_t offset = default_layout_field(lo, s) + (s << DEFAULT_SCALE);
        return mul_hi(h, lv.num_slices) + (offset & lv.l_mask);
    }

    /* Returns the output of a key in the first level: with the default
       shifts we use constants, so that the compiler can fuse the scaling of
       the seed with the addition, and we extract the field of the layout
       with a load. */
    __attribute__((always_inline)) inline uint64_t pos0(uint64_t h, uint64_t lo, uint64_t s) const {
        level_params const& lv = m_params0;
        if (m_default_shifts) return pos_default(lv, h, lo, s);
        switch (m_fast_scale) {
            case 1:
                return pos(lv, h, lo, s, DEFAULT_LAYOUT_SHIFT, 0);
            case 2:
                return pos(lv, h, lo, s, DEFAULT_LAYOUT_SHIFT, 1);
            case 4:
                return pos(lv, h, lo, s, DEFAULT_LAYOUT_SHIFT, 3);
            default:
                return pos(lv, h, lo, s, m_layout_shift, static_cast<uint32_t>(lv.scale));
        }
    }

    /* Handles keys bumped from the first level, given the signature for the
       first level and the second hash of the key. */
    __attribute__((noinline)) uint64_t get_slow(uint64_t h0, uint64_t second) const {
        for (auto const& lv : m_params) {
            const uint64_t h = level_sig(h0, second, lv.salt);
            const __uint128_t p = static_cast<__uint128_t>(h) * lv.buckets;
            const uint64_t s = m_seeds.get(lv.first_seed + static_cast<uint64_t>(p >> 64));
            if (s != 0) {
                const uint64_t position = pos(lv, h, static_cast<uint64_t>(p), s, m_layout_shift,
                                              static_cast<uint32_t>(lv.scale));
                return m_remap.access(lv.offset + position);
            }
        }
        /* Only keys outside the original set can reach this point. */
        return 0;
    }

    template <typename Visitor, typename T>
    static void visit_impl(Visitor& visitor, T&& t) {
        visitor.visit(t.m_seed);
        visitor.visit(t.m_hasher);
        visitor.visit(t.m_second_hasher);
        visitor.visit(t.m_num_keys);
        visitor.visit(t.m_layout_shift);
        visitor.visit(t.m_default_shifts);
        visitor.visit(t.m_fast_scale);
        visitor.visit(t.m_params0);
        visitor.visit(t.m_seeds0);
        visitor.visit(t.m_params);
        visitor.visit(t.m_seeds);
        visitor.visit(t.m_remap);
    }
};

}  // namespace phastr
