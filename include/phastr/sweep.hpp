#pragma once

/*
    The sweep assigning seeds to the buckets of a level.

    Buckets enter, in order of index, a priority queue holding a few buckets
    at a time (the window), and the bucket of highest priority gets the
    feasible seed of minimum cost: the product of the distances of the slots
    of its keys from its *origin*, a point slightly before its slices.

    The lowest log2(R) bits of the seed of a bucket, where R is the number
    of layouts, choose a *layout* r, and the remaining bits a *rotation* j.
    Once the layout is chosen, each key of the bucket has a *base slot*, its
    slot for rotation 0, and rotation j moves all the keys of the bucket by
    j *strides*, wrapping around the end of their slices. As the rotation
    varies, a key goes through the slots of its *ring*: the slots of its
    slice whose distance from its base slot is a multiple of the stride.

    The set of used slots is stored by residue classes modulo the stride
    (*rows*), so the bits of the ring of a key are consecutive bits of a
    row: the free rotations of a layout, which map every key of the bucket
    to a free slot, are obtained by cyclically shifting the bits of the ring
    of each key by the ring position of the key and combining the results
    with a bitwise OR.
*/

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <queue>

#include "util.hpp"
#include "geometry.hpp"

namespace phastr {

/* Size of the cyclic set recording the buckets in the window. */
static constexpr size_t WINDOW_BITS = 1024;

/* Number of keys from which the buckets of a window are kept in a heap. */
static constexpr size_t LARGE_BUCKET = 64;

/* The maximum number of free rotations of a bucket that are examined
   exhaustively when looking for the best seed. */
static constexpr uint32_t MAX_FREE = 32;

/* The buckets in the window of a sweep, in order of priority.

   The priority of a bucket depends on its size, and decreases with its
   index. Since buckets enter the window in order of index, the buckets of
   each size form a queue, and the bucket with the highest priority is the
   first bucket of one of the queues. Buckets with at least LARGE_BUCKET
   keys, if any, are kept in a heap. */
struct window {
    /* The priority weights for buckets with less than LARGE_BUCKET keys. */
    std::array<int64_t, LARGE_BUCKET> weights_by_size;
    /* For each size below LARGE_BUCKET, a cyclic buffer containing the
       lowest bits of the indices of the buckets of that size. */
    std::vector<std::array<uint8_t, WINDOW>> queues;
    /* The position in its cyclic buffer of the first bucket of each
       queue. */
    std::array<uint8_t, LARGE_BUCKET> first;
    /* The length of each queue. */
    std::array<uint16_t, LARGE_BUCKET> len;
    /* The key of the first bucket of each nonempty queue. */
    std::array<int64_t, LARGE_BUCKET> keys;
    /* The sizes of the nonempty queues, as a set of bits. */
    uint64_t nonempty = 0;
    /* The keys of the buckets with at least LARGE_BUCKET keys. */
    std::priority_queue<int64_t> large;
    /* The buckets in the window, as a cyclic set of bits. */
    std::array<uint64_t, WINDOW_BITS / 64> buckets;

    explicit window(std::array<int64_t, 7> const& weights) {
        static_assert(WINDOW <= 256 && WINDOW <= WINDOW_BITS, "");
        for (size_t size = 0; size != LARGE_BUCKET; ++size) {
            weights_by_size[size] = weight(weights, size);
        }
        queues.resize(LARGE_BUCKET);
        first.fill(0);
        len.fill(0);
        keys.fill(0);
        buckets.fill(0);
    }

    /* Returns the priority weight of the buckets of the given size: weights
       are given for the first seven sizes, and extrapolated linearly for
       the following ones (clamped to +-2^40, so that keys do not overflow
       for fewer than about 2^43 buckets). */
    static int64_t weight(std::array<int64_t, 7> const& weights, size_t size) {
        if (size <= 7) return weights[std::max<size_t>(size, 1) - 1];
        __int128 w =
            static_cast<__int128>(weights[6] - weights[5]) * static_cast<int64_t>(size - 7) +
            weights[6];
        const __int128 clamp = __int128(1) << 40;
        if (w > clamp) w = clamp;
        if (w < -clamp) w = -clamp;
        return static_cast<int64_t>(w);
    }

    /* Returns the key of a bucket: its priority and, in the lowest bits,
       the complement of the lowest bits of its index. */
    static inline int64_t key(int64_t weight, uint64_t b) {
        /* The shift is performed on the unsigned representation, as shifting
           a negative value is undefined behavior in C++17. */
        const uint64_t priority = static_cast<uint64_t>(weight - 1024 * static_cast<int64_t>(b));
        return static_cast<int64_t>((priority << 10) | (WINDOW_BITS - 1 - b % WINDOW_BITS));
    }

    inline bool is_empty() const {
        return nonempty == 0 && large.empty();
    }

    /* Returns the first bucket of the window in [begin, end), or `end` if
       there is none, assuming that all the buckets of the window are in
       [end - WINDOW, end). */
    inline uint64_t first_in(uint64_t begin, uint64_t end) const {
        uint64_t b = begin;
        uint64_t word = buckets[(b % WINDOW_BITS) / 64] >> (b % 64);
        while (true) {
            if (word != 0) {
                const uint64_t f = b + __builtin_ctzll(word);
                return f < end ? f : end;
            }
            b = (b | 63) + 1;
            if (b >= end) return end;
            word = buckets[(b % WINDOW_BITS) / 64];
        }
    }

    /* Adds bucket b, which must follow all buckets added previously and
       have the given nonzero size. */
    inline void push(uint64_t b, size_t size, std::array<int64_t, 7> const& weights) {
        buckets[(b % WINDOW_BITS) / 64] |= uint64_t(1) << (b % 64);
        if (size >= LARGE_BUCKET) {
            large.push(key(weight(weights, size), b));
            return;
        }
        if (len[size] == 0) {
            keys[size] = key(weights_by_size[size], b);
            nonempty |= uint64_t(1) << size;
        }
        const size_t last = static_cast<size_t>(first[size]) + len[size];
        queues[size][last % WINDOW] = static_cast<uint8_t>(b);
        len[size] += 1;
    }

    /* Removes and returns the bucket with the highest priority (there must
       be one), given the oldest bucket in the window. */
    inline uint64_t pop(uint64_t oldest) {
        int64_t best_key = INT64_MIN;
        size_t size = 0;
        uint64_t ne = nonempty;
        while (ne != 0) {
            const size_t s = __builtin_ctzll(ne);
            ne &= ne - 1;
            if (keys[s] > best_key) {
                best_key = keys[s];
                size = s;
            }
        }
        /* Buckets are identified by the lowest bits of their index. */
        auto bucket_from_low = [oldest](uint64_t low, uint64_t bits) {
            return oldest + (low - oldest) % bits;
        };
        uint64_t b;
        if (!large.empty() && large.top() > best_key) {
            const int64_t k = large.top();
            large.pop();
            b = bucket_from_low(WINDOW_BITS - 1 - static_cast<uint64_t>(k) % WINDOW_BITS,
                                WINDOW_BITS);
        } else {
            assert(size != 0);
            auto const& queue = queues[size];
            b = bucket_from_low(queue[first[size] % WINDOW], 256);
            first[size] = static_cast<uint8_t>(first[size] + 1);
            len[size] -= 1;
            if (len[size] == 0) {
                nonempty &= ~(uint64_t(1) << size);
            } else {
                const uint64_t next = bucket_from_low(queue[first[size] % WINDOW], 256);
                keys[size] = key(weights_by_size[size], next);
            }
        }
        buckets[(b % WINDOW_BITS) / 64] &= ~(uint64_t(1) << (b % 64));
        return b;
    }
};

/* The set of used slots of a level while its sweeps, which can run in
   parallel, update it: bit i of word w is set if slot 64w + i is used.

   The words are kept in a plain array, as atomics are not movable, so
   they cannot live in a std::vector (libc++ rejects it). */
struct shared_used_slots {
    std::unique_ptr<std::atomic<uint64_t>[]> words;
    size_t num_words;

    explicit shared_used_slots(uint64_t m)
        : words(new std::atomic<uint64_t>[ceil_div<uint64_t>(m, 64)])
        , num_words(ceil_div<uint64_t>(m, 64)) {
        for (size_t i = 0; i != num_words; ++i) {
            words[i].store(0, std::memory_order_relaxed);
        }
    }

    inline void add(size_t w, uint64_t bits) {
        words[w].fetch_or(bits, std::memory_order_relaxed);
    }

    /* A plain view of the words, valid once the sweeps are done. */
    std::vector<uint64_t> take() {
        std::vector<uint64_t> out(num_words);
        for (size_t i = 0; i != num_words; ++i) {
            out[i] = words[i].load(std::memory_order_relaxed);
        }
        words.reset();
        num_words = 0;
        return out;
    }
};

/* Returns whether slot p is used, given the bit vector of used slots. */
static inline bool is_occupied(std::vector<uint64_t> const& bits, uint64_t p) {
    return (bits[p / 64] >> (p % 64)) & 1;
}

/* Calls f, in increasing order, on the free slots in [0, m) of the bit
   vector of used slots. */
template <typename F>
static inline void for_each_hole(std::vector<uint64_t> const& bits, uint64_t m, F const& f) {
    for (size_t w = 0; w != bits.size(); ++w) {
        uint64_t free = ~bits[w];
        const uint64_t valid = m - w * 64;
        if (valid < 64) free &= (uint64_t(1) << valid) - 1;
        while (free != 0) {
            f(w * 64 + __builtin_ctzll(free));
            free &= free - 1;
        }
    }
}

/* The result of the sweep of a level. */
template <typename SeedType>
struct level_output {
    std::vector<SeedType> seeds;
    std::vector<uint64_t> occupied;
};

/* The parameters of the rings of a level, and of the set of used slots of
   a sweep. The set of used slots is stored by residue classes modulo the
   stride (rows), 64 slots of each row at a time (columns): bit i of word
   c * 2^log2_stride + tau is associated with slot (64c + i) * 2^log2_stride
   + tau (the number of columns is a power of two, and columns are used
   cyclically). */
struct rings {
    /* The slice length minus one. */
    uint64_t l_mask = 1023;
    /* The base-2 logarithm of the amount u by which a unit increase of the
       seed moves the keys of a bucket. */
    uint32_t scale = 2;
    /* The number of layouts. */
    size_t layouts = 4;
    /* The width f of the field of each layout in the value providing the
       offsets (64 divided by the number of layouts). */
    uint32_t layout_bits = 16;
    /* The base-2 logarithm of the stride (at most that of the slice
       length). */
    uint32_t log2_stride = 4;
    /* The number of slots of a ring. */
    size_t len = 64;
    /* The number of rotations of a layout (at least the length of a ring;
       if it is larger, rotations are redundant). */
    size_t rotations = 64;
    /* The number of words of the set of used slots, minus one. */
    size_t word_mask = 0;

    static rings make(geometry const& g) {
        rings r;
        const uint64_t l = g.l_mask + 1;
        /* With slices shorter than the stride, a ring is a single slot. */
        r.log2_stride = std::min<uint32_t>(g.log2_layouts + g.scale, ilog2(l));
        /* The cyclic set must contain the slots reachable from the buckets
           in the window plus a slice; gap sweeps need three slices more.
           Moreover, slots that are no longer reachable are cleared a column
           at a time, so there must be room for the slots of a column. */
        const uint64_t per_bucket = ceil_div<uint64_t>(g.num_slices, g.buckets);
        const uint64_t slots = next_power_of_two(4 * (l + 1) + 2 * WINDOW * per_bucket +
                                                 (uint64_t(64) << r.log2_stride));
        r.l_mask = g.l_mask;
        r.scale = g.scale;
        r.layouts = size_t(1) << g.log2_layouts;
        r.layout_bits = 64 >> g.log2_layouts;
        r.len = static_cast<size_t>(l >> r.log2_stride);
        r.rotations = (size_t(1) << g.seed_bits) >> g.log2_layouts;
        r.word_mask = static_cast<size_t>(slots / 64 - 1);
        return r;
    }

    /* Whether this is the default configuration (8-bit seeds, four layouts,
       slices of 1024 slots), for which the sweep compiles a version in
       which shifts and masks are constants. */
    bool is_default() const {
        return l_mask == 1023 && scale == 2 && layouts == 4 && layout_bits == 16 &&
               log2_stride == 4 && len == 64 && rotations == 64;
    }

    /* Returns the offset in its slice of the base slot of a key in layout
       r, given the value providing its offsets. */
    inline size_t offset(uint64_t offsets, size_t r) const {
        return static_cast<size_t>(
            ((offsets >> (r * layout_bits)) + (static_cast<uint64_t>(r) << scale)) & l_mask);
    }

    /* Returns the first slot of the ring of a key in a layout and the ring
       position of the key, given the beginning of the slice of the key and
       the offset of its base slot. */
    inline void ring(uint64_t slice_begin, size_t offset, uint64_t& first, size_t& x) const {
        first = slice_begin + (offset & ((size_t(1) << log2_stride) - 1));
        x = offset >> log2_stride;
    }

    /* Returns the slot of a key for rotation j of a layout, given the first
       slot of its ring and the ring position of the key. */
    inline uint64_t slot(uint64_t first, size_t index, size_t j) const {
        return first + (static_cast<uint64_t>((index + j) & (len - 1)) << log2_stride);
    }

    /* Returns the seed choosing layout r and rotation j. Rotation 0 of
       layout 0 is seed zero, which marks bumped buckets; if rotations are
       redundant we use instead the rotation equal to the length of a ring,
       which maps the keys to the same slots. */
    inline size_t seed(size_t r, size_t j) const {
        if (r == 0 && j == 0) {
            assert(len < rotations);
            return len * layouts;
        }
        return j * layouts + r;
    }

    /* Returns the index of the word of the set of used slots containing the
       bit associated with slot p (the index of the bit in the word is
       p >> log2_stride, modulo 64). */
    inline size_t word(uint64_t p) const {
        const uint64_t stride_mask = (uint64_t(1) << log2_stride) - 1;
        return static_cast<size_t>((((p >> 6) & ~stride_mask) | (p & stride_mask)) & word_mask);
    }

    /* Returns the bits of the 64 slots of the row of slot p starting from p
       (that is, bit i is associated with the slot p plus i strides) in the
       cyclic set of used slots. */
    inline uint64_t row64(uint64_t const* used, uint64_t p) const {
        const uint32_t t = log2_stride;
        const size_t w = word(p);
        const uint64_t lo = used[w];
        const uint64_t hi = used[(w + (size_t(1) << t)) & word_mask];
        return static_cast<uint64_t>(
            (((static_cast<__uint128_t>(hi) << 64) | lo) >> ((p >> t) % 64)));
    }
};

/* The parts loaded by a sweep, restricted to a range of buckets: the
   buckets of the window are consecutive, so a few parts are enough.

   A `Source` provides the signatures of the keys of a level, distributed
   into parts of consecutive buckets, which sweeps load in increasing
   order. It must provide:
     - using loader_type = ...;              (per-sweep loader state)
     - uint32_t log2_min_buckets() const;
     - size_t part(uint64_t b) const;
     - uint64_t first_bucket(size_t p) const;
     - uint64_t buckets(size_t p, geometry const& g) const;
     - void load(loader_type&, size_t p, geometry const& g,
                 uint64_t range_begin, uint64_t range_end,
                 std::vector<uint64_t>& sigs, std::vector<size_t>& begin,
                 std::vector<uint64_t>& temp) const;
   where `load` distributes into buckets the signatures of part p whose
   bucket is in the range, writing them to `sigs` and the position in `sigs`
   of the first signature of each bucket of the range, followed by the
   number of signatures, to `begin`. */
template <typename Source>
struct parts_cache {
    struct part {
        size_t index = SIZE_MAX;
        uint64_t first_bucket = 0;
        uint64_t end_bucket = 0;
        std::vector<uint64_t> sigs;
        std::vector<size_t> begin;
    };

    uint64_t range_begin, range_end;
    std::vector<part> parts;
    std::vector<uint64_t> temp;
    typename Source::loader_type loader;

    parts_cache(Source const& source, uint64_t begin, uint64_t end)
        : range_begin(begin), range_end(end) {
        /* The window can span (WINDOW >> log2_min_buckets) + 1 parts, and
           we keep the last one when loading the next. */
        const size_t slots = (WINDOW >> source.log2_min_buckets()) + 2;
        parts.resize(slots);
    }

    /* Loads the part of bucket b, if necessary, replacing the oldest
       loaded part. */
    inline void ensure(Source const& source, geometry const& g, uint64_t b) {
        const size_t p = source.part(b);
        for (auto const& x : parts) {
            if (x.index == p) return;
        }
        load(source, g, p);
    }

    void load(Source const& source, geometry const& g, size_t p) {
        /* An unused slot, or the one of the oldest part (parts are loaded
           in increasing order). */
        size_t slot = SIZE_MAX;
        for (size_t i = 0; i != parts.size(); ++i) {
            if (parts[i].index == SIZE_MAX) {
                slot = i;
                break;
            }
        }
        if (slot == SIZE_MAX) {
            slot = 0;
            for (size_t i = 1; i != parts.size(); ++i) {
                if (parts[i].index < parts[slot].index) slot = i;
            }
        }
        part& x = parts[slot];
        const uint64_t first = std::max(source.first_bucket(p), range_begin);
        const uint64_t end = std::min(source.first_bucket(p) + source.buckets(p, g), range_end);
        source.load(loader, p, g, first, end, x.sigs, x.begin, temp);
        x.index = p;
        x.first_bucket = first;
        x.end_bucket = end;
    }

    inline part const& part_of(Source const& source, uint64_t b) const {
        const size_t p = source.part(b);
        for (auto const& x : parts) {
            if (x.index == p) return x;
        }
        assert(false);
        return parts[0];
    }

    /* Returns the size of bucket b, loading its part if necessary. */
    inline size_t size(Source const& source, geometry const& g, uint64_t b) {
        ensure(source, g, b);
        auto const& x = part_of(source, b);
        const size_t i = static_cast<size_t>(b - x.first_bucket);
        return x.begin[i + 1] - x.begin[i];
    }

    /* Returns the first bucket from b on that is not empty, or the end of
       the range. */
    uint64_t next_nonempty(Source const& source, geometry const& g, uint64_t b) {
        while (b < range_end && size(source, g, b) == 0) b += 1;
        return b;
    }

    /* Calls f(b, size) on each bucket of [begin, end), loading parts as
       necessary (a part is looked up just once). */
    template <typename F>
    inline void for_each_size(Source const& source, geometry const& g, uint64_t begin, uint64_t end,
                              F const& f) {
        uint64_t b = begin;
        while (b < end) {
            ensure(source, g, b);
            auto const& x = part_of(source, b);
            const uint64_t stop = std::min(end, x.end_bucket);
            const uint64_t first = x.first_bucket;
            for (uint64_t i = b; i != stop; ++i) {
                const size_t k = static_cast<size_t>(i - first);
                f(i, x.begin[k + 1] - x.begin[k]);
            }
            b = stop;
        }
    }

    /* Returns the signatures of the keys of bucket b, whose part must be
       loaded. */
    inline void keys(Source const& source, uint64_t b, uint64_t const*& begin,
                     uint64_t const*& end) const {
        auto const& x = part_of(source, b);
        const size_t i = static_cast<size_t>(b - x.first_bucket);
        begin = x.sigs.data() + x.begin[i];
        end = x.sigs.data() + x.begin[i + 1];
    }
};

/* The state of a sweep over a range of buckets. */
template <typename Source, typename SeedType>
struct sweep {
    Source const* sigs;
    geometry const* g;
    std::array<int64_t, 7> const* weights;
    /* First bucket of the range, and end of the range. */
    uint64_t lo, hi;
    /* Seeds of the buckets in the range. */
    SeedType* seeds;
    /* Cyclic bit set of used slots (see rings). */
    std::vector<uint64_t> used;
    rings rg;
    /* The first slot of the first column of the set of used slots; slots
       before this one are no longer reachable. */
    uint64_t first_slot;
    /* The set of used slots of the level, to which the slots that are no
       longer reachable are added. */
    shared_used_slots* occupied;
    /* A buffer for the words of the set of used slots of the level
       corresponding to a column. */
    std::vector<uint64_t> column;
    /* For each layout, the free rotations of the bucket being searched. */
    std::vector<uint64_t> free;
    /* The nonzero words of free, each with its index. */
    std::vector<std::pair<uint64_t, uint32_t>> nonzero;
    /* For each key of the bucket being searched and each layout, the first
       slot of the ring of the key minus the origin of the bucket (upper 32
       bits) and the ring position of the key (lower 32 bits). */
    std::vector<uint64_t> ring_keys;
    /* The base-2 logarithms of the distances of slots from the origin of
       their bucket, with LOG2_FRACTION fractional bits. */
    std::vector<uint32_t> log2_table;
    /* The rotations at which some key of the bucket being searched wraps
       around the end of its slice. */
    std::vector<uint32_t> back;

    sweep(Source const& source, geometry const& geom, std::array<int64_t, 7> const& w, uint64_t lo_,
          uint64_t hi_, SeedType* seeds_, shared_used_slots& occ)
        : sigs(&source), g(&geom), weights(&w), lo(lo_), hi(hi_), seeds(seeds_), occupied(&occ) {
        rg = rings::make(geom);
        const size_t free_words = rg.layouts * ceil_div<size_t>(rg.len, 64);
        /* Columns contain 64 strides. */
        first_slot = (geom.first_slice(lo) >> (rg.log2_stride + 6)) << (rg.log2_stride + 6);
        const uint64_t max_distance = ORIGIN_OFFSET + geom.max_slice_distance() + geom.l_mask;
        used.assign(rg.word_mask + 1, 0);
        column.assign(size_t(1) << rg.log2_stride, 0);
        free.reserve(free_words);
        nonzero.assign(free_words, {0, 0});
        ring_keys.reserve(64 * rg.layouts);
        /* For distances smaller than 2^18, the exact values are at least
           10^-5 away from the midpoints between integers, so the table does
           not depend on the accuracy of log2. */
        log2_table.resize(static_cast<size_t>(max_distance) + 1);
        for (uint64_t d = 0; d <= max_distance; ++d) {
            log2_table[d] = static_cast<uint32_t>(
                std::nearbyint(std::log2(static_cast<double>(std::max<uint64_t>(d, 1))) *
                               static_cast<double>(uint64_t(1) << LOG2_FRACTION)));
        }
        back.reserve(64);
    }

    /* Marks slot p as used. */
    inline void set(uint64_t p) {
        used[rg.word(p)] |= uint64_t(1) << ((p >> rg.log2_stride) % 64);
    }

    /* Removes the first column from the set of used slots, adding its slots
       to the set of used slots of the level. */
    template <typename R>
    inline void retire_column(R const& rings_) {
        const uint32_t t = rings_.log2_stride();
        const size_t w = rings_.word(first_slot);
        for (size_t row = 0; row != (size_t(1) << t); ++row) {
            uint64_t word = used[w + row];
            used[w + row] = 0;
            while (word != 0) {
                const size_t p = (static_cast<size_t>(__builtin_ctzll(word)) << t) + row;
                column[p / 64] |= uint64_t(1) << (p % 64);
                word &= word - 1;
            }
        }
        /* Columns start at multiples of 64 strides. */
        const size_t first_word = static_cast<size_t>(first_slot / 64);
        for (size_t i = 0; i != column.size(); ++i) {
            if (column[i] != 0) {
                occupied->add(first_word + i, column[i]);
                column[i] = 0;
            }
        }
        first_slot += uint64_t(64) << t;
    }

    /* Returns the cost of rotation j of layout r for the bucket being
       searched: the sum of the base-2 logarithms of the distances of the
       slots of its keys from the origin of the bucket (that is, the
       logarithm of the product of the distances). The logarithms are in
       fixed point and read from a table, so the cost does not depend on
       the order of the keys. */
    template <typename R>
    inline uint64_t cost(R const& rings_, size_t r, size_t j) const {
        uint64_t sum = 0;
        const size_t layouts = rings_.layouts();
        for (size_t i = 0; i < ring_keys.size(); i += layouts) {
            const uint64_t d = ring_keys[i + r];
            const size_t x =
                (static_cast<size_t>(static_cast<uint32_t>(d)) + j) & (rings_.len() - 1);
            sum += log2_table[static_cast<size_t>(d >> 32) + (x << rings_.log2_stride())];
        }
        return sum;
    }

    /* Finds the seed of bucket b of minimum cost, given the keys of the
       bucket, and marks their slots as used; returns zero if no seed is
       feasible. */
    template <typename R>
    inline size_t search(R const& rings_, uint64_t const* keys, uint64_t const* keys_end,
                         uint64_t b) {
        const size_t n = rings_.len();
        const size_t layouts = rings_.layouts();
        const size_t words = ceil_div<size_t>(n, 64);
        const uint64_t ring_mask = n < 64 ? (uint64_t(1) << n) - 1 : ~uint64_t(0);
        const uint64_t origin = g->first_slice(b) - ORIGIN_OFFSET; /* may wrap */

        free.clear();
        free.resize(layouts * words, 0);
        /* Rotation 0 of layout 0 is seed zero, which marks bumped buckets,
           unless rotations are redundant (see rings::seed). */
        free[0] = (n == rings_.rotations_()) ? 1 : 0;
        const size_t num_keys = static_cast<size_t>(keys_end - keys);
        ring_keys.clear();
        ring_keys.resize(num_keys * layouts, 0);

        /* For each layout we cyclically shift the bits of the ring of each
           key by the ring position of the key, so that bit j is associated
           with the slot of the key for rotation j, and combine the results:
           we obtain the rotations for which some key is mapped to a used
           slot. */
        uint64_t const* used_ptr = used.data();
        for (size_t k = 0; k != num_keys; ++k) {
            const uint64_t key = keys[k];
            const uint64_t slice_begin = g->slice_begin(key);
            const uint64_t offsets = g->offsets(key);
            uint64_t* ring_key = ring_keys.data() + k * layouts;
            for (size_t r = 0; r != layouts; ++r) {
                const size_t offset = rings_.offset(offsets, r);
                uint64_t first;
                size_t x;
                rings_.ring(slice_begin, offset, first, x);
                ring_key[r] = ((first - origin) << 32) | static_cast<uint64_t>(x);
                if (words == 1) {
                    /* Rings of at most a word (e.g., 8-bit seeds and four
                       layouts). */
                    const uint64_t ring = rings_.row64(used_ptr, first) & ring_mask;
                    free[r] |= (ring >> x) | (ring << ((n - x) % 64));
                } else {
                    /* Rings are sequences of whole words. */
                    auto word_at = [&](size_t w) {
                        return rings_.row64(used_ptr,
                                            first + (static_cast<uint64_t>(64 * (w & (words - 1)))
                                                     << rings_.log2_stride()));
                    };
                    const size_t xw = x / 64, xb = x % 64;
                    uint64_t prev = word_at(xw);
                    for (size_t w = 0; w != words; ++w) {
                        const uint64_t next = word_at(xw + w + 1);
                        free[r * words + w] |= xb == 0 ? prev : (prev >> xb) | (next << (64 - xb));
                        prev = next;
                    }
                }
            }
        }

        /* We complement the result, and gather the nonzero words. */
        size_t count = 0;
        uint32_t total = 0;
        for (size_t i = 0; i != free.size(); ++i) {
            const uint64_t w = ~free[i] & ring_mask;
            free[i] = w;
            nonzero[count] = {w, static_cast<uint32_t>(i)};
            count += (w != 0) ? 1 : 0;
            total += static_cast<uint32_t>(__builtin_popcountll(w));
        }
        if (total == 0) return 0;

        /* The best seed is identified by the index of its bit in free. */
        size_t best;
        if (total <= MAX_FREE) {
            /* Just a few free rotations (the common case): we consider all
               of them, with a single loop that has no other branches. */
            uint64_t best_cost = UINT64_MAX;
            best = 0;
            size_t i = 0;
            for (uint32_t it = 0; it != total; ++it) {
                auto& entry = nonzero[i];
                const uint64_t word = entry.first;
                const size_t bit = static_cast<size_t>(entry.second) * 64 + __builtin_ctzll(word);
                const uint64_t c = cost(rings_, bit / (64 * words), bit % (64 * words));
                if (c < best_cost) {
                    best_cost = c;
                    best = bit;
                }
                const uint64_t next_word = word & (word - 1);
                entry.first = next_word;
                i += (next_word == 0) ? 1 : 0;
            }
        } else {
            best = search_many(rings_);
        }

        const size_t r = best / (64 * words);
        const size_t j = best % (64 * words);
        if (place(rings_, keys, keys_end, r, j)) return rings_.seed(r, j);
        return search_distinct(rings_, keys, keys_end);
    }

    /* Returns the seed of minimum cost among the free rotations of the
       bucket being searched when there are many free rotations.

       In a layout the cost increases with the rotation, except at the
       rotations at which some key wraps around the end of its slice:
       between two such rotations only the first free one can be the best
       one. */
    template <typename R>
    size_t search_many(R const& rings_) {
        const size_t n = rings_.len();
        const size_t layouts = rings_.layouts();
        const size_t words = ceil_div<size_t>(n, 64);
        uint64_t best_cost = UINT64_MAX;
        size_t best = 0;
        for (size_t r = 0; r != layouts; ++r) {
            uint64_t const* free_r = free.data() + r * words;
            back.clear();
            for (size_t i = 0; i < ring_keys.size(); i += layouts) {
                const size_t x = static_cast<size_t>(static_cast<uint32_t>(ring_keys[i + r]));
                if (x != 0) back.push_back(static_cast<uint32_t>(n - x));
            }
            std::sort(back.begin(), back.end());
            back.erase(std::unique(back.begin(), back.end()), back.end());
            size_t begin = 0;
            for (size_t i = 0; i <= back.size(); ++i) {
                const size_t end = i < back.size() ? back[i] : n;
                const size_t j = next_set(free_r, begin, end);
                if (j != end) {
                    const uint64_t c = cost(rings_, r, j);
                    const size_t idx = r * 64 * words + j;
                    if (c < best_cost || (c == best_cost && idx < best)) {
                        best_cost = c;
                        best = idx;
                    }
                }
                begin = end;
            }
        }
        return best;
    }

    /* Marks as used the slots of the given keys for rotation j of layout r,
       which must be free, unless two keys are mapped to the same slot, in
       which case nothing happens and the method returns false. */
    template <typename R>
    inline bool place(R const& rings_, uint64_t const* keys, uint64_t const* keys_end, size_t r,
                      size_t j) {
        auto bit_of = [&](uint64_t key, size_t& w, uint64_t& mask) {
            const size_t offset = rings_.offset(g->offsets(key), r);
            uint64_t first;
            size_t x;
            rings_.ring(g->slice_begin(key), offset, first, x);
            const uint64_t p = rings_.slot(first, x, j);
            w = rings_.word(p);
            mask = uint64_t(1) << ((p >> rings_.log2_stride()) % 64);
        };
        /* The slots were free: a used slot has been marked by a previous
           key of the bucket. */
        uint64_t collisions = 0;
        for (uint64_t const* k = keys; k != keys_end; ++k) {
            size_t w;
            uint64_t mask;
            bit_of(*k, w, mask);
            collisions |= used[w] & mask;
            used[w] |= mask;
        }
        if (collisions != 0) {
            for (uint64_t const* k = keys; k != keys_end; ++k) {
                size_t w;
                uint64_t mask;
                bit_of(*k, w, mask);
                used[w] &= ~mask;
            }
        }
        return collisions == 0;
    }

    /* Like search, but considers all free rotations in order of cost until
       one that maps the keys of the bucket to distinct slots is found (this
       is rarely necessary). */
    template <typename R>
    size_t search_distinct(R const& rings_, uint64_t const* keys, uint64_t const* keys_end) {
        /* Keys with the same signature are mapped to the same slot by every
           seed, so the bucket must be bumped. */
        std::vector<uint64_t> sorted(keys, keys_end);
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 1; i < sorted.size(); ++i) {
            if (sorted[i - 1] == sorted[i]) return 0;
        }
        const size_t ring_bits = 64 * ceil_div<size_t>(rings_.len(), 64);
        std::vector<std::tuple<uint64_t, size_t, size_t>> candidates;
        for (size_t i = 0; i != free.size(); ++i) {
            uint64_t word = free[i];
            while (word != 0) {
                const size_t bit = i * 64 + __builtin_ctzll(word);
                word &= word - 1;
                const size_t r = bit / ring_bits;
                const size_t j = bit % ring_bits;
                candidates.emplace_back(cost(rings_, r, j), r, j);
            }
        }
        std::sort(candidates.begin(), candidates.end());
        for (auto const& [c, r, j] : candidates) {
            (void)c;
            if (place(rings_, keys, keys_end, r, j)) return rings_.seed(r, j);
        }
        return 0;
    }

    /* Processes the buckets of the range; returns false if allow_bump is
       false and some bucket could not be placed (in which case the sweep
       stops immediately). */
    bool run(bool allow_bump);

private:
    template <typename R>
    bool sweep_range(R const& rings_, bool allow_bump);
};

/* A view over the parameters of the rings: the generic version reads them
   from the rings struct, whereas the default version compiles them as
   constants (the parameters are used as shift amounts and masks in the
   innermost loops). */
struct dynamic_rings_view {
    rings const* r;
    explicit dynamic_rings_view(rings const& rg) : r(&rg) {}

    inline uint64_t l_mask() const {
        return r->l_mask;
    }
    inline uint32_t scale() const {
        return r->scale;
    }
    inline size_t layouts() const {
        return r->layouts;
    }
    inline uint32_t layout_bits() const {
        return r->layout_bits;
    }
    inline uint32_t log2_stride() const {
        return r->log2_stride;
    }
    inline size_t len() const {
        return r->len;
    }
    inline size_t rotations_() const {
        return r->rotations;
    }
    inline size_t word_mask() const {
        return r->word_mask;
    }

    inline size_t offset(uint64_t offsets, size_t layout) const {
        return static_cast<size_t>(
            ((offsets >> (layout * layout_bits())) + (static_cast<uint64_t>(layout) << scale())) &
            l_mask());
    }
    inline void ring(uint64_t slice_begin, size_t offset, uint64_t& first, size_t& x) const {
        first = slice_begin + (offset & ((size_t(1) << log2_stride()) - 1));
        x = offset >> log2_stride();
    }
    inline uint64_t slot(uint64_t first, size_t index, size_t j) const {
        return first + (static_cast<uint64_t>((index + j) & (len() - 1)) << log2_stride());
    }
    inline size_t seed(size_t layout, size_t j) const {
        if (layout == 0 && j == 0) return len() * layouts();
        return j * layouts() + layout;
    }
    inline size_t word(uint64_t p) const {
        const uint64_t stride_mask = (uint64_t(1) << log2_stride()) - 1;
        return static_cast<size_t>((((p >> 6) & ~stride_mask) | (p & stride_mask)) & word_mask());
    }
    inline uint64_t row64(uint64_t const* used, uint64_t p) const {
        const size_t w = word(p);
        const uint64_t lo = used[w];
        const uint64_t hi = used[(w + (size_t(1) << log2_stride())) & word_mask()];
        return static_cast<uint64_t>(
            (((static_cast<__uint128_t>(hi) << 64) | lo) >> ((p >> log2_stride()) % 64)));
    }
};

/* The default configuration (8-bit seeds, four layouts, slices of 1024
   slots) with constant parameters, except for the size of the set of used
   slots. */
struct default_rings_view {
    size_t m_word_mask;
    explicit default_rings_view(rings const& rg) : m_word_mask(rg.word_mask) {}

    static inline uint64_t l_mask() {
        return 1023;
    }
    static inline uint32_t scale() {
        return 2;
    }
    static inline size_t layouts() {
        return 4;
    }
    static inline uint32_t layout_bits() {
        return 16;
    }
    static inline uint32_t log2_stride() {
        return 4;
    }
    static inline size_t len() {
        return 64;
    }
    static inline size_t rotations_() {
        return 64;
    }
    inline size_t word_mask() const {
        return m_word_mask;
    }

    inline size_t offset(uint64_t offsets, size_t layout) const {
        return static_cast<size_t>(
            ((offsets >> (layout * layout_bits())) + (static_cast<uint64_t>(layout) << scale())) &
            l_mask());
    }
    inline void ring(uint64_t slice_begin, size_t offset, uint64_t& first, size_t& x) const {
        first = slice_begin + (offset & ((size_t(1) << log2_stride()) - 1));
        x = offset >> log2_stride();
    }
    inline uint64_t slot(uint64_t first, size_t index, size_t j) const {
        return first + (static_cast<uint64_t>((index + j) & (len() - 1)) << log2_stride());
    }
    inline size_t seed(size_t layout, size_t j) const {
        /* Rotations are never redundant with the default parameters. */
        return j * layouts() + layout;
    }
    inline size_t word(uint64_t p) const {
        const uint64_t stride_mask = (uint64_t(1) << log2_stride()) - 1;
        return static_cast<size_t>((((p >> 6) & ~stride_mask) | (p & stride_mask)) & word_mask());
    }
    inline uint64_t row64(uint64_t const* used, uint64_t p) const {
        const size_t w = word(p);
        const uint64_t lo = used[w];
        const uint64_t hi = used[(w + (size_t(1) << log2_stride())) & word_mask()];
        return static_cast<uint64_t>(
            (((static_cast<__uint128_t>(hi) << 64) | lo) >> ((p >> log2_stride()) % 64)));
    }
};

template <typename Source, typename SeedType>
template <typename R>
bool sweep<Source, SeedType>::sweep_range(R const& rings_, bool allow_bump) {
    window win(*weights);
    parts_cache<Source> parts(*sigs, lo, hi);
    const uint64_t column_slots = uint64_t(64) << rings_.log2_stride();
    /* The buckets in [lo, next) have entered the window, which holds those
       not yet placed; oldest is the first of them. */
    uint64_t oldest = lo, next = lo;
    while (true) {
        if (win.is_empty()) {
            /* The window restarts from the next nonempty bucket. */
            oldest = parts.next_nonempty(*sigs, *g, next);
            if (oldest == hi) break;
            next = oldest;
        }
        /* The columns before the first slice of the oldest bucket contain
           slots that are no longer reachable. */
        while (first_slot + column_slots <= g->first_slice(oldest)) retire_column(rings_);
        /* The buckets up to WINDOW buckets after the oldest one enter. */
        const uint64_t end = std::min(oldest + WINDOW, hi);
        parts.for_each_size(*sigs, *g, next, end, [&](uint64_t b, size_t size) {
            if (size != 0) win.push(b, size, *weights);
        });
        next = end;
        /* We place buckets in order of priority until the oldest one is
           placed, so that the window can move forward. */
        while (true) {
            const uint64_t b = win.pop(oldest);
            uint64_t const *kb, *ke;
            parts.keys(*sigs, b, kb, ke);
            const size_t seed = search(rings_, kb, ke, b);
            seeds[b - lo] = static_cast<SeedType>(seed);
            if (seed == 0 && !allow_bump) return false;
            if (b == oldest) break;
        }
        const uint64_t f = win.first_in(oldest + 1, next);
        if (f != next) oldest = f;
    }
    /* The remaining columns. */
    for (size_t i = 0; i <= (rings_.word_mask() >> rings_.log2_stride()); ++i) {
        retire_column(rings_);
    }
    return true;
}

template <typename Source, typename SeedType>
bool sweep<Source, SeedType>::run(bool allow_bump) {
    if (rg.is_default()) return sweep_range(default_rings_view(rg), allow_bump);
    return sweep_range(dynamic_rings_view(rg), allow_bump);
}

/* Assigns seeds to the buckets of a level, possibly in parallel, given the
   signatures of its keys. The level is split into chunks, which are swept
   possibly in parallel; then, the gaps left between the chunks are swept,
   possibly in parallel. Returns false if allow_bump is false and some
   bucket could not be placed. */
template <typename SeedType, typename Source>
static bool sweep_level(Source const& sigs, geometry const& g,
                        std::array<int64_t, 7> const& weights, bool allow_bump,
                        uint64_t num_threads, level_output<SeedType>& out) {
    const uint64_t nb = g.buckets;
    const uint64_t chunks = g.num_sweep_chunks();

    std::vector<SeedType> seeds(nb, 0);
    /* Each sweep adds to the set the slots it uses (and those marked as
       used beforehand). */
    shared_used_slots occupied(g.m);

    if (chunks == 1) {
        sweep<Source, SeedType> sw(sigs, g, weights, 0, nb, seeds.data(), occupied);
        if (!sw.run(allow_bump)) return false;
        out.seeds = std::move(seeds);
        out.occupied = occupied.take();
        return true;
    }

    /* With several chunks, the gap is at most the number of buckets.
       Chunk i processes [bounds[i], bounds[i + 1] - gap), except for the
       last one, which processes everything. */
    const uint64_t gap = g.chunk_gap();
    std::vector<uint64_t> bounds(chunks + 1);
    for (uint64_t i = 0; i <= chunks; ++i) {
        bounds[i] = static_cast<uint64_t>(static_cast<__uint128_t>(i) * nb / chunks);
    }
    std::atomic<bool> failed{false};
    parallel_for_tasks(chunks, num_threads, [&](size_t i) {
        if (failed.load(std::memory_order_relaxed)) return;
        const uint64_t chunk_lo = bounds[i];
        const uint64_t chunk_hi = (i + 1 == chunks) ? bounds[i + 1] : bounds[i + 1] - gap;
        sweep<Source, SeedType> sw(sigs, g, weights, chunk_lo, chunk_hi, seeds.data() + chunk_lo,
                                   occupied);
        if (!sw.run(allow_bump)) failed.store(true, std::memory_order_relaxed);
    });
    if (failed.load()) return false;

    /* Gaps: the seeds of the neighboring buckets are now known, and the
       slots they use are marked. */
    std::vector<std::vector<SeedType>> gap_seeds(chunks - 1);
    parallel_for_tasks(chunks - 1, num_threads, [&](size_t i) {
        if (failed.load(std::memory_order_relaxed)) return;
        const uint64_t gap_lo = bounds[i + 1] - gap;
        const uint64_t gap_hi = bounds[i + 1];
        gap_seeds[i].assign(gap_hi - gap_lo, 0);
        sweep<Source, SeedType> sw(sigs, g, weights, gap_lo, gap_hi, gap_seeds[i].data(), occupied);
        const uint64_t before = std::max(gap_lo >= gap ? gap_lo - gap : 0, bounds[i]);
        const uint64_t after = std::min(gap_hi + gap, bounds[i + 2]);
        const uint64_t first = g.first_slice(gap_lo);
        /* These buckets have been swept already. */
        parts_cache<Source> parts(sigs, before, after);
        auto mark = [&](uint64_t b) {
            parts.ensure(sigs, g, b);
            uint64_t const *kb, *ke;
            parts.keys(sigs, b, kb, ke);
            for (uint64_t const* x = kb; x != ke; ++x) {
                const size_t s = static_cast<size_t>(seeds[g.bucket(*x)]);
                if (s != 0) {
                    const uint64_t p = g.pos(*x, s);
                    if (p >= first) sw.set(p);
                }
            }
        };
        for (uint64_t b = before; b != gap_lo; ++b) mark(b);
        for (uint64_t b = gap_hi; b != after; ++b) mark(b);
        if (!sw.run(allow_bump)) failed.store(true, std::memory_order_relaxed);
    });
    if (failed.load()) return false;
    for (size_t i = 0; i != chunks - 1; ++i) {
        std::copy(gap_seeds[i].begin(), gap_seeds[i].end(), seeds.begin() + (bounds[i + 1] - gap));
    }
    out.seeds = std::move(seeds);
    out.occupied = occupied.take();
    return true;
}

}  // namespace phastr
