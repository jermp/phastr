#pragma once

/*
    The construction of the levels of a phastr::function.

    Keys are read once: for each key we store a record containing its
    signature for the first level and its second hash, from which its
    signature for every level is computed (see store.hpp). The records of
    the keys bumped from a level are stored for the following level.
    Records are kept in memory or, in external-memory mode, in temporary
    files; the structure built is identical, and does not depend on the
    number of threads (it depends on the maximum number of sweep chunks).
*/

#include <iostream>
#include <tuple>

#include "store.hpp"
#include "partitioned.hpp"
#include "sweep.hpp"

namespace phastr {

/* The output of the construction of the levels, assembled into a
   phastr::function by its build methods. */
struct build_result {
    uint64_t num_keys = 0;
    level_params params0;
    /* The seeds of the first level: as bytes when seeds have at most eight
       bits, as 16-bit values otherwise (one of the two is empty). */
    std::vector<uint8_t> seeds0_bytes;
    std::vector<uint16_t> seeds0_wide;
    /* The parameters of the following levels, with first_seed set. */
    std::vector<level_params> params;
    /* The seeds of the following levels, concatenated. */
    std::vector<uint16_t> seeds;
    /* The remapping: for each output of each level after the first one, the
       hole of the first level it is mapped to (a nondecreasing sequence
       with values in [0, num_keys)). */
    std::vector<uint64_t> remap_values;
};

namespace detail {

/* Applies f to disjoint ranges of the partitions of a store, one for each
   thread, possibly in parallel; f(t, partition_begin, partition_end) is
   called for thread index t. */
template <typename F>
static void by_thread(store const& st, uint64_t num_threads, F const& f) {
    const size_t np = st.num_partitions();
    const size_t threads = std::min<size_t>(std::max<uint64_t>(num_threads, 1), np);
    parallel_for_tasks(threads, threads,
                       [&](size_t t) { f(t, t * np / threads, (t + 1) * np / threads); });
}

/* Assigns seeds to the buckets of a level whose records are in a store. */
template <typename SeedType>
static level_output<SeedType> sweep_store(store const& st, geometry const& g,
                                          std::array<int64_t, 7> const& weights,
                                          uint64_t num_threads) {
    partitioned parts(st, g);
    level_output<SeedType> out;
    const bool ok = sweep_level<SeedType>(parts, g, weights, true, num_threads, out);
    (void)ok;
    assert(ok);
    return out;
}

/* The records of the keys bumped from a level: in a new store, or in a
   vector if the following level is the last one. */
struct bumped_keys {
    bool last = false;
    store st;
    std::vector<record> records;

    uint64_t size() const {
        return last ? records.size() : st.len;
    }
};

/* Returns the records of the k keys of the bumped buckets, given the salt
   of the following level. */
template <typename SeedType>
static bumped_keys collect_bumped(store const& st, geometry const& g,
                                  std::vector<SeedType> const& seeds, uint64_t k, uint64_t salt,
                                  build_configuration const& config) {
    const sig_fn sig = st.sig;
    bumped_keys out;
    if (k <= LAST_LEVEL_THRESHOLD) {
        out.last = true;
        std::vector<std::vector<record>> parts(
            std::min<size_t>(std::max<uint64_t>(config.num_threads, 1), st.num_partitions()));
        by_thread(st, config.num_threads, [&](size_t t, size_t begin, size_t end) {
            std::vector<record> buf;
            for (size_t p = begin; p != end; ++p) {
                st.for_each(p, buf, [&](record const& r) {
                    if (seeds[g.bucket(sig.sig(r))] == 0) parts[t].push_back(r);
                });
            }
        });
        for (auto& part : parts) {
            out.records.insert(out.records.end(), part.begin(), part.end());
        }
        assert(out.records.size() == k);
    } else {
        store_writer writer(sig_fn::salted(salt), log2_partitions_for(k), config.external_memory,
                            config.tmp_dir);
        std::mutex len_mutex;
        uint64_t len = 0;
        by_thread(st, config.num_threads, [&](size_t, size_t begin, size_t end) {
            store_buffers buffers(writer);
            std::vector<record> buf;
            for (size_t p = begin; p != end; ++p) {
                st.for_each(p, buf, [&](record const& r) {
                    if (seeds[g.bucket(sig.sig(r))] == 0) buffers.push(r);
                });
            }
            const uint64_t pushed = buffers.finish();
            std::lock_guard<std::mutex> guard(len_mutex);
            len += pushed;
        });
        assert(len == k);
        out.st = writer.finish(len);
    }
    return out;
}

/* Returns whether two records of a store are equal: as keys are no longer
   available, we consider two keys duplicates if they have the same
   signature for the first level and the same second hash (equal keys are
   always bumped from the first level, so they are detected in the second
   one). */
static inline bool has_duplicates(store const& st, uint64_t num_threads) {
    std::atomic<bool> found{false};
    by_thread(st, num_threads, [&](size_t, size_t begin, size_t end) {
        std::vector<record> all, buf;
        for (size_t p = begin; p != end; ++p) {
            if (found.load(std::memory_order_relaxed)) return;
            all.clear();
            st.for_each(p, buf, [&](record const& r) { all.push_back(r); });
            std::sort(all.begin(), all.end());
            for (size_t i = 1; i < all.size(); ++i) {
                if (all[i - 1] == all[i]) {
                    found.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        }
    });
    return found.load();
}

static inline bool has_duplicates(std::vector<record> const& records) {
    std::vector<record> all = records;
    std::sort(all.begin(), all.end());
    for (size_t i = 1; i < all.size(); ++i) {
        if (all[i - 1] == all[i]) return true;
    }
    return false;
}

/* Builds an in-memory store over the given records with the given
   signature function (used by the last level, whose record set is small). */
static inline store store_records(std::vector<record> const& records, sig_fn sig,
                                  uint32_t log2_partitions) {
    store st;
    st.sig = sig;
    st.log2_partitions = log2_partitions;
    st.len = records.size();
    st.on_disk = false;
    st.partitions.assign(size_t(1) << log2_partitions, {});
    for (auto const& r : records) {
        st.partitions[partition_of(sig.sig(r), log2_partitions)].push_back(r);
    }
    return st;
}

/* Builds the last level, of index `index`, on the given records, which does
   not bump keys: we change the salt until we succeed, enlarging the output
   range every eight attempts. Returns the parameters of the level, its
   seeds, its used slots, and its output range. */
static inline std::tuple<level_params, level_output<uint16_t>, uint64_t> last_level(
    build_configuration const& config, uint64_t index, std::vector<record> const& records,
    std::array<int64_t, 7> const& weights) {
    const uint64_t k = records.size();
    for (uint64_t attempt = 0; attempt != LAST_LEVEL_ATTEMPTS; ++attempt) {
        const uint64_t m = k + k / 4 + 16 + (attempt / 8) * (k / 8 + 8);
        const geometry g = geometry::make(config, k, m, std::min(config.bucket_size, 3.0));
        const uint64_t salt = level_salt(config.seed, index, attempt);
        const store st = store_records(records, sig_fn::salted(salt), log2_partitions_for(k));
        partitioned parts(st, g);
        level_output<uint16_t> out;
        if (sweep_level<uint16_t>(parts, g, weights, false, config.num_threads, out)) {
            level_params level = g.level();
            level.salt = salt;
            return {level, std::move(out), g.m};
        }
    }
    throw std::runtime_error(
        "phastr: could not build the last level: the slice length or the number of bits of a "
        "seed is probably too small");
}

static inline uint64_t popcount_words(std::vector<uint64_t> const& words) {
    uint64_t count = 0;
    for (uint64_t w : words) count += static_cast<uint64_t>(__builtin_popcountll(w));
    return count;
}

static inline void log_level(build_configuration const& config, uint64_t index, uint64_t k,
                             uint64_t bumped) {
    if (!config.verbose) return;
    std::cout << "phastr: level " << index << ": " << k << " keys, " << bumped << " bumped ("
              << (100.0 * static_cast<double>(bumped) /
                  static_cast<double>(std::max<uint64_t>(k, 1)))
              << "%)" << std::endl;
}

/* Builds the levels on the n keys whose records are in the given store
   (which is consumed). */
template <typename SeedType0>
static build_result build_levels(build_configuration const& config, store&& level0_store,
                                 uint64_t n) {
    const std::array<int64_t, 7> weights = config.priority_weights();
    build_result res;
    res.num_keys = n;

    if (n == 0) {
        /* A single empty level, so that queries need no special case. */
        const geometry g = geometry::make(config, 0, 1, config.bucket_size);
        res.params0 = g.level();
        if (config.seed_bits <= 8) {
            res.seeds0_bytes.assign(1, 0);
        } else {
            res.seeds0_wide.assign(1, 0);
        }
        return res;
    }

    /* The first level. */
    const geometry geom0 = geometry::make(config, n, n, config.bucket_size);
    res.params0 = geom0.level();
    std::vector<uint64_t> holes;
    bumped_keys cur;
    {
        store st = std::move(level0_store);
        level_output<SeedType0> out =
            sweep_store<SeedType0>(st, geom0, weights, config.num_threads);
        const uint64_t num_holes = n - popcount_words(out.occupied);
        holes.reserve(num_holes);
        for_each_hole(out.occupied, n, [&](uint64_t p) { holes.push_back(p); });
        assert(holes.size() == num_holes);
        out.occupied.clear();
        out.occupied.shrink_to_fit();
        log_level(config, 0, n, num_holes);
        cur =
            collect_bumped(st, geom0, out.seeds, num_holes, level_salt(config.seed, 1, 0), config);
        if (config.seed_bits <= 8) {
            res.seeds0_bytes.assign(out.seeds.begin(), out.seeds.end());
        } else {
            res.seeds0_wide.assign(out.seeds.begin(), out.seeds.end());
        }
    }

    /* The following levels, keeping their sets of used slots and their
       output ranges for the remapping. */
    std::vector<std::pair<std::vector<uint64_t>, uint64_t>> used;
    std::vector<std::pair<level_params, std::vector<uint16_t>>> levels;
    uint64_t offset = 0;
    while (true) {
        const uint64_t index = levels.size() + 1;
        level_params level;
        std::vector<uint16_t> seeds;
        std::vector<uint64_t> occupied;
        bumped_keys next;
        uint64_t k, m;
        if (!cur.last) {
            store st = std::move(cur.st);
            k = st.len;
            const uint64_t salt = level_salt(config.seed, index, 0);
            const geometry g = geometry::make(config, k, k, config.bucket_size);
            /* Equal keys are bumped from the first level, so they are all
               in the second one. */
            if (index == 1 && has_duplicates(st, config.num_threads)) {
                throw std::runtime_error("phastr: duplicate keys");
            }
            level_output<uint16_t> out = sweep_store<uint16_t>(st, g, weights, config.num_threads);
            const uint64_t num_used = popcount_words(out.occupied);
            if (k - num_used >= k) {
                throw std::runtime_error(
                    "phastr: no bucket of a level could be placed: the expected bucket size "
                    "is too large for the slice length");
            }
            next = collect_bumped(st, g, out.seeds, k - num_used,
                                  level_salt(config.seed, index + 1, 0), config);
            level = g.level();
            level.salt = salt;
            seeds = std::move(out.seeds);
            occupied = std::move(out.occupied);
            m = g.m;
        } else {
            if (cur.records.empty()) break;
            k = cur.records.size();
            if (index == 1 && has_duplicates(cur.records)) {
                throw std::runtime_error("phastr: duplicate keys");
            }
            auto [last_params, out, last_m] = last_level(config, index, cur.records, weights);
            level = last_params;
            seeds = std::move(out.seeds);
            occupied = std::move(out.occupied);
            next.last = true;
            m = last_m;
        }
        log_level(config, index, k, next.size());
        level.offset = offset;
        offset += m;
        levels.emplace_back(level, std::move(seeds));
        used.emplace_back(std::move(occupied), m);
        cur = std::move(next);
    }

    /* The remapping: the used slots of the following levels are mapped, in
       order, to the free slots of the first level, and their free slots to
       the last free slot assigned. */
    res.remap_values.reserve(offset);
    size_t next_hole = 0;
    uint64_t last_hole = 0;
    for (auto const& [occupied, m] : used) {
        for (uint64_t p = 0; p != m; ++p) {
            if (is_occupied(occupied, p)) {
                assert(next_hole < holes.size());
                last_hole = holes[next_hole];
                next_hole += 1;
            }
            res.remap_values.push_back(last_hole);
        }
    }
    assert(next_hole == holes.size());

    /* Concatenate the seeds of the following levels. */
    res.params.reserve(levels.size());
    for (auto& [level, seeds] : levels) {
        level.first_seed = res.seeds.size();
        res.seeds.insert(res.seeds.end(), seeds.begin(), seeds.end());
        res.params.push_back(level);
    }
    return res;
}

}  // namespace detail

}  // namespace phastr
