#pragma once

/*
    The parts of a level whose records are in a store: each part is made of
    consecutive partitions, so that it contains at least 2 * WINDOW buckets
    (unless there is a single part), and part p contains the buckets from
    the bucket of the first signature of its first partition (included) to
    that of the first signature of the following part (excluded).

    The keys of the first bucket of a part can thus have their signature in
    the previous part: they are passed by the loader of the previous part,
    if it was loaded just before, or found by reading it.
*/

#include "store.hpp"
#include "sweep.hpp"

namespace phastr {

/* The base-2 logarithm of the number of keys of the smaller parts into
   which `distribute` distributes signatures before distributing them into
   buckets: with their copy and the positions of their buckets, they take
   about 150 KB, which fits the second-level cache of any architecture. */
static constexpr uint32_t LOG2_CACHE_KEYS = 13;

/* The base-2 logarithm of the maximum number of smaller parts. */
static constexpr uint32_t MAX_LOG2_SUBPARTS = 9;

/* The number of standard deviations of slack in the size of a subpart. */
static constexpr double SLACK = 6.0;

/* Distributes into buckets a sequence of runs of signatures whose buckets
   are consecutive and start from first_bucket. The signatures are written
   to `sigs`, and the position of the first signature of each bucket to
   `begin`, assuming that `sigs` starts at position `offset`. */
template <typename Runs>
static inline void into_buckets(Runs const& source, uint64_t* sigs, size_t* begin,
                                size_t num_buckets, uint64_t first_bucket, size_t offset,
                                geometry const& g) {
    for (size_t b = 0; b != num_buckets; ++b) begin[b] = 0;
    for (auto const& run : source) {
        for (uint64_t h : run) begin[g.bucket(h) - first_bucket] += 1;
    }
    size_t sum = 0;
    for (size_t b = 0; b != num_buckets; ++b) {
        const size_t size = begin[b];
        begin[b] = sum;
        sum += size;
    }
    for (auto const& run : source) {
        for (uint64_t h : run) {
            size_t& next = begin[g.bucket(h) - first_bucket];
            sigs[next] = h;
            next += 1;
        }
    }
    /* Each element is now the end of its bucket, that is, the beginning of
       the following one. */
    size_t prev = 0;
    for (size_t b = 0; b != num_buckets; ++b) {
        const size_t end = begin[b];
        begin[b] = offset + prev;
        prev = end;
    }
}

/* A lightweight span of signatures. */
struct sig_run {
    uint64_t const* data = nullptr;
    size_t size = 0;
    uint64_t const* begin() const {
        return data;
    }
    uint64_t const* end() const {
        return data + size;
    }
};

/* Distributes into buckets a sequence of runs of signatures whose buckets
   are in [range_begin, range_end), writing them to `sigs` and the position
   in `sigs` of the first signature of each bucket of the range, followed by
   the number of signatures, to `begin`; `temp` is a temporary buffer.

   Signatures are distributed first into smaller parts that fit the cache,
   and then each smaller part into its buckets. */
static inline void distribute(std::vector<sig_run> const& source, geometry const& g,
                              uint64_t range_begin, uint64_t range_end, std::vector<uint64_t>& sigs,
                              std::vector<size_t>& begin, std::vector<uint64_t>& temp) {
    const uint64_t first_bucket = range_begin;
    const size_t num_buckets = static_cast<size_t>(range_end - range_begin);
    size_t len = 0;
    for (auto const& run : source) len += run.size;
    /* Buffers are reused, so we avoid clearing them. */
    if (sigs.size() < len) sigs.resize(len);
    begin.clear();
    begin.resize(num_buckets + 1, 0);
    const uint32_t log2_subparts =
        std::min({ilog2(next_power_of_two(len >> LOG2_CACHE_KEYS)), MAX_LOG2_SUBPARTS,
                  ilog2(std::max<uint64_t>(num_buckets, 1))});
    if (log2_subparts == 0) {
        into_buckets(source, sigs.data(), begin.data(), num_buckets, first_bucket, 0, g);
        begin[num_buckets] = len;
        return;
    }
    /* The smaller parts are made of 2^sub_shift consecutive buckets,
       starting from the first bucket of the range; we reserve for each of
       them its expected size plus some slack, and we keep separately the
       signatures that do not fit. */
    const uint32_t sub_shift =
        ilog2(num_buckets) + 1 >= log2_subparts + 1 ? ilog2(num_buckets) - log2_subparts : 0;
    const size_t subparts = ((num_buckets - 1) >> sub_shift) + 1;
    const double mean = static_cast<double>(len) *
                        static_cast<double>(std::min<size_t>(size_t(1) << sub_shift, num_buckets)) /
                        static_cast<double>(num_buckets);
    const size_t capacity = static_cast<size_t>(std::ceil(mean + SLACK * std::sqrt(mean))) + 32;
    if (sigs.size() < subparts * capacity) sigs.resize(subparts * capacity);
    std::vector<size_t> next(subparts);
    for (size_t s = 0; s != subparts; ++s) next[s] = s * capacity;
    std::vector<std::pair<size_t, uint64_t>> extra;
    for (auto const& run : source) {
        for (uint64_t h : run) {
            const size_t s = static_cast<size_t>(g.bucket(h) - first_bucket) >> sub_shift;
            if (next[s] < (s + 1) * capacity) {
                sigs[next[s]] = h;
                next[s] += 1;
            } else {
                extra.emplace_back(s, h);
            }
        }
    }
    std::sort(extra.begin(), extra.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });
    std::vector<uint64_t> extra_sigs(extra.size());
    for (size_t i = 0; i != extra.size(); ++i) extra_sigs[i] = extra[i].second;
    if (temp.size() < len) temp.resize(len);
    size_t offset = 0;
    size_t extra_pos = 0;
    for (size_t s = 0; s != subparts; ++s) {
        const size_t sub_begin = s << sub_shift;
        const size_t sub_buckets =
            std::min<size_t>(size_t(1) << sub_shift, num_buckets - sub_begin);
        size_t extra_end = extra_pos;
        while (extra_end != extra.size() && extra[extra_end].first == s) extra_end += 1;
        const std::vector<sig_run> runs = {{sigs.data() + s * capacity, next[s] - s * capacity},
                                           {extra_sigs.data() + extra_pos, extra_end - extra_pos}};
        const size_t sub_len = runs[0].size + runs[1].size;
        into_buckets(runs, temp.data() + offset, begin.data() + sub_begin, sub_buckets,
                     first_bucket + sub_begin, offset, g);
        offset += sub_len;
        extra_pos = extra_end;
    }
    begin[num_buckets] = len;
    std::swap(sigs, temp);
}

/* The state of a loader of the parts of a partitioned level. */
struct partitioned_loader {
    /* The part following the last loaded one (SIZE_MAX if none). */
    size_t next = SIZE_MAX;
    /* The signatures of the keys of the first bucket of part `next` found
       in the partitions of the last loaded part. */
    std::vector<uint64_t> carry;
    /* The signatures to distribute into buckets. */
    std::vector<uint64_t> sigs;
    /* A buffer for reading records. */
    std::vector<record> records;
};

/* The parts of a level whose records are in a store (see the comment at the
   top of this file). Models the Source concept of parts_cache. */
struct partitioned {
    using loader_type = partitioned_loader;

    store const* st;
    /* The base-2 logarithm of the number of parts. */
    uint32_t log2_parts;
    /* The first bucket of each part, followed by the number of buckets. */
    std::vector<uint64_t> first;
    /* The number of buckets. */
    uint64_t num_buckets;
    uint32_t m_log2_min_buckets;

    partitioned(store const& s, geometry const& g) : st(&s) {
        log2_parts = s.log2_partitions;
        while (log2_parts > 0 && (g.buckets >> log2_parts) < 2 * WINDOW) log2_parts -= 1;
        const size_t np = size_t(1) << log2_parts;
        first.resize(np + 1);
        first[0] = 0;
        for (size_t p = 1; p != np; ++p) {
            first[p] = g.bucket(static_cast<uint64_t>(p) << (64 - log2_parts));
        }
        first[np] = g.buckets;
        uint64_t min_buckets = UINT64_MAX;
        for (size_t p = 0; p != np; ++p) {
            min_buckets = std::min(min_buckets, first[p + 1] - first[p]);
        }
        num_buckets = g.buckets;
        m_log2_min_buckets = ilog2(std::max<uint64_t>(min_buckets, 1));
    }

    inline uint32_t log2_min_buckets() const {
        return m_log2_min_buckets;
    }

    inline size_t part(uint64_t b) const {
        /* The largest p such that the first bucket of part p, that is,
           floor(p * buckets / 2^log2_parts), is at most b. */
        return static_cast<size_t>((((static_cast<__uint128_t>(b + 1)) << log2_parts) - 1) /
                                   num_buckets);
    }

    inline uint64_t first_bucket(size_t p) const {
        return first[p];
    }

    inline uint64_t buckets(size_t p, geometry const&) const {
        return first[p + 1] - first[p];
    }

    /* Calls f on the signatures of the keys whose record is in partition q
       of the store. */
    template <typename F>
    void read(size_t q, std::vector<record>& records, F const& f) const {
        const sig_fn sig = st->sig;
        st->for_each(q, records, [&](record const& r) { f(sig.sig(r)); });
    }

    void load(loader_type& loader, size_t p, geometry const& g, uint64_t range_begin,
              uint64_t range_end, std::vector<uint64_t>& sigs, std::vector<size_t>& begin,
              std::vector<uint64_t>& temp) const {
        const uint64_t part_first = first[p];
        const uint64_t part_end = first[p + 1];
        /* The base-2 logarithm of the number of partitions of a part. */
        const uint32_t c = st->log2_partitions - log2_parts;
        auto& buf = loader.sigs;
        buf.clear();
        /* The keys of the first bucket whose signature is in the previous
           part. */
        if (p > 0 && range_begin <= part_first && part_first < range_end) {
            if (loader.next == p) {
                buf.insert(buf.end(), loader.carry.begin(), loader.carry.end());
            } else {
                for (size_t q = (p - 1) << c; q != p << c; ++q) {
                    read(q, loader.records, [&](uint64_t h) {
                        if (g.bucket(h) == part_first) buf.push_back(h);
                    });
                }
            }
        }
        /* The keys of the last bucket of a part (the first of the following
           one) can have their signature in the following part. */
        loader.carry.clear();
        for (size_t q = p << c; q != (p + 1) << c; ++q) {
            read(q, loader.records, [&](uint64_t h) {
                const uint64_t b = g.bucket(h);
                if (b == part_end) {
                    loader.carry.push_back(h);
                } else if (range_begin <= b && b < range_end) {
                    buf.push_back(h);
                }
            });
        }
        loader.next = p + 1;
        const std::vector<sig_run> runs = {{buf.data(), buf.size()}};
        distribute(runs, g, range_begin, range_end, sigs, begin, temp);
    }
};

}  // namespace phastr
