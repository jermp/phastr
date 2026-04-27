#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <queue>
#include <utility>
#include <vector>

#include "conf.hpp"
#include "cyclic.hpp"
#include "evaluator.hpp"
#include "seed_chooser.hpp"

namespace phast {

/*
    Single-thread PHast level builder.

    Inputs:
      - keys (vector of u64 hashes), sorted by `core.bucket_for(key)`.
      - core (a generic_core describing buckets, slices, output range).
      - evaluator (weights for picking the next bucket to activate).
      - seed_size (8 bits per seed by default).
      - seed_chooser (SeedOnly).

    Output:
      - seeds vector (one entry per bucket); 0 means "bumped".
      - bucket_begin (cumulative bucket sizes, used by `unassigned_values`).

    The implementation follows the PHast paper: a sliding window of `WINDOW_SIZE`
    buckets is maintained as a max-heap keyed by `evaluator.eval(bucket_nr, size)`.
    At each step the bucket with the highest score is activated. A cyclic bitset
    `used_values` tracks which output positions have been claimed.
*/

/* Compute bucket_begin = exclusive prefix sum of bucket sizes, given keys are
   already grouped by bucket (ascending). */
inline std::vector<size_t> bucket_begin_st(uint64_t const* keys, size_t n,
                                           generic_core const& core) {
    std::vector<size_t> b(core.buckets_num + 1, 0);
    for (size_t i = 0; i < n; ++i) ++b[core.bucket_for(keys[i])];
    size_t sum = 0;
    for (size_t i = 0; i <= core.buckets_num; ++i) {
        size_t inc = b[i];
        b[i] = sum;
        sum += inc;
    }
    return b;
}

template <typename SeedSize, typename SeedChooser>
struct builder_state {
    SeedSize seed_size;
    SeedChooser chooser;

    generic_core core;
    weights eval;
    uint64_t const* keys = nullptr;
    size_t num_keys = 0;
    std::vector<size_t> bucket_begin;  // size = buckets_num + 1
    uint16_t span_limit = WINDOW_SIZE;

    // Builder state below:
    std::vector<typename SeedSize::vec_element> seeds;
    typename SeedChooser::used_values uv;
    size_t span_begin = 0;
    size_t value_to_clear = 0;
    cyclic_set<MAX_WINDOW_SIZE / 64> in_window;

    // Max-heap of (score, bucket_nr); ties broken by smaller bucket_nr first.
    struct heap_entry {
        int64_t score;
        size_t bucket;
        bool operator<(heap_entry const& o) const {
            if (score != o.score) return score < o.score;     // max-heap on score
            return bucket > o.bucket;                          // tie: smaller bucket first
        }
    };
    std::priority_queue<heap_entry> pq;

    builder_state(uint64_t const* keys_, size_t n_, generic_core const& core_,
                  SeedSize ss, SeedChooser sc, weights w_)
        : seed_size(ss)
        , chooser(sc)
        , core(core_)
        , eval(w_)
        , keys(keys_)
        , num_keys(n_)
        , bucket_begin(bucket_begin_st(keys_, n_, core_))
        , seeds(ss.new_zeroed(core_.buckets_num)) {}

    inline size_t bucket_size(size_t bucket) const {
        return bucket_begin[bucket + 1] - bucket_begin[bucket];
    }
    inline bool bucket_is_empty(size_t bucket) const {
        return bucket_begin[bucket + 1] == bucket_begin[bucket];
    }
    /* slice_begin of any key in `bucket` (all keys in a bucket share the same slice begin
       up to a small range, but the common reference is the smallest hash for that bucket). */
    inline uint64_t slice_begin_of_bucket(size_t bucket) const {
        return core.slice_begin(keys[bucket_begin[bucket]]);
    }
    inline size_t span_end() const {
        return std::min<size_t>(span_begin + span_limit, core.buckets_num);
    }
    bool find_nonempty() {
        while (span_begin < core.buckets_num && bucket_is_empty(span_begin)) ++span_begin;
        return span_begin < core.buckets_num;
    }
    void add_candidates_from(size_t first_to_add) {
        size_t end = span_end();
        for (size_t b = first_to_add; b < end; ++b) {
            size_t sz = bucket_size(b);
            if (sz == 0) continue;
            pq.push({eval.eval(b, sz), b});
            in_window.add(b);
        }
    }
    void clear_used() {
        size_t end = slice_begin_of_bucket(span_begin);
        while (value_to_clear != end) {
            uv.remove(value_to_clear);
            ++value_to_clear;
        }
    }

    /* Run the builder. */
    void build() {
        if (!find_nonempty()) return;
        value_to_clear = slice_begin_of_bucket(span_begin);
        add_candidates_from(span_begin);
        while (!pq.empty()) {
            heap_entry top = pq.top();
            pq.pop();
            size_t best_bucket = top.bucket;
            in_window.remove(best_bucket);
            size_t sz = bucket_size(best_bucket);
            uint16_t best_seed = SeedChooser::best_seed_search(
                uv, &keys[bucket_begin[best_bucket]], sz, core, SeedSize::bits_per_seed);
            seed_size.set(seeds.data(), best_bucket, best_seed);
            if (best_bucket == span_begin) {
                size_t old_span_end = span_end();
                ++span_begin;
                while (span_begin < old_span_end && !in_window.contain(span_begin)) ++span_begin;
                if (span_begin == old_span_end) {
                    if (!find_nonempty()) return;
                }
                clear_used();
                add_candidates_from(old_span_end);
            }
        }
    }

    /* Returns (unassigned_values bitmap of size output_range, count of 1-bits). */
    std::pair<std::vector<uint64_t>, size_t> unassigned_values() const {
        size_t out_range = core.output_range(SeedChooser::extra_shift(SeedSize::bits_per_seed));
        size_t words = (out_range + 63) / 64;
        std::vector<uint64_t> bm(words, ~uint64_t(0));
        if (out_range % 64 != 0) bm.back() >>= (64 - (out_range % 64));
        size_t unassigned_len = out_range;
        for (size_t b = 0; b < core.buckets_num; ++b) {
            uint16_t s = seed_size.get(seeds.data(), b);
            if (SeedChooser::BUMPING && s == 0) continue;
            size_t lo = bucket_begin[b], hi = bucket_begin[b + 1];
            for (size_t i = lo; i < hi; ++i) {
                size_t pos = SeedChooser::f(keys[i], s, core);
                bm[pos >> 6] &= ~(1ULL << (pos & 63));
            }
            unassigned_len -= (hi - lo);
        }
        return {std::move(bm), unassigned_len};
    }
};

/* Build a single PHast level: return seeds and unassigned_values bitmap.
   `out_range` is the output range of this level. */
template <typename SeedSize, typename SeedChooser>
struct level_result {
    std::vector<typename SeedSize::vec_element> seeds;
    std::vector<uint64_t> unassigned_bm;
    size_t unassigned_len;
};

template <typename SeedSize, typename SeedChooser>
level_result<SeedSize, SeedChooser> build_level_st(uint64_t const* keys, size_t n,
                                                   generic_core const& core, SeedSize seed_size,
                                                   SeedChooser chooser, weights const& w) {
    builder_state<SeedSize, SeedChooser> st(keys, n, core, seed_size, chooser, w);
    st.build();
    auto [bm, ul] = st.unassigned_values();
    return level_result<SeedSize, SeedChooser>{std::move(st.seeds), std::move(bm), ul};
}

}  // namespace phast
