#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

#include "essentials.hpp"

#include "builder.hpp"
#include "compressed_array.hpp"
#include "conf.hpp"
#include "hasher.hpp"
#include "seed_chooser.hpp"
#include "seeds.hpp"

namespace phast {

/*
    Top-level PHast minimal-perfect hash function.

    Template parameters:
      Hasher           - seeded hash function with hash_one(key, level_nr) -> uint64_t
      SeedSize         - seed packing strategy (default: 8 bits per seed)
      SeedChooser      - seed selection policy (default: SeedOnly = regular PHast)
      CompressedArray  - storage for the level mapping (default: Elias-Fano)

    Construction reads keys, splits them into "levels":
      - Level 0 places most keys at slots in [0, output_range_0).
      - Bumped keys (seed == 0) go to level 1, then level 2, ...
      - Each level's "unassigned" output positions in level 0 form the targets
        for the bumped keys placed at higher levels.

    A compressed array (Elias-Fano by default) maps higher-level output positions
    to the original level-0 free slots, giving a minimal perfect hash function.
*/

namespace detail {

/* Iterator over the 1-bits of a bitmap, in ascending order. */
struct bit_ones_iterator {
    uint64_t const* words;
    size_t len_words;
    size_t cur_word_idx;
    uint64_t cur_word;

    bit_ones_iterator(uint64_t const* w, size_t lw) : words(w), len_words(lw) {
        cur_word_idx = 0;
        cur_word = lw ? words[0] : 0;
    }

    bool next(size_t& out) {
        while (cur_word == 0) {
            ++cur_word_idx;
            if (cur_word_idx >= len_words) return false;
            cur_word = words[cur_word_idx];
        }
        size_t bit = __builtin_ctzll(cur_word);
        out = cur_word_idx * 64 + bit;
        cur_word &= cur_word - 1;
        return true;
    }
};

inline bool bit_get(std::vector<uint64_t> const& bm, size_t i) {
    return (bm[i >> 6] >> (i & 63)) & 1ULL;
}

}  // namespace detail

template <typename Hasher = default_hasher,                      //
          typename SeedSize = bits8_seed_size,                   //
          typename SeedChooser = seed_only,                      //
          typename CompressedArray = default_compressed_array>   //
struct function {
    using hash_type = uint64_t;
    using seed_size_type = SeedSize;
    using vec_element = typename SeedSize::vec_element;

    /* PHast is always a minimal perfect hash function. */
    static constexpr bool minimal = true;

    /* Build configuration. */
    struct config {
        uint16_t bucket_size_100 = bits_per_seed_to_100_bucket_size(SeedSize::bits_per_seed);
        uint16_t preferred_slice_len = 0;  // 0 means "choose automatically"
    };

    function() = default;

    /* Build the function from a vector of keys. Keys must not contain duplicates. */
    template <typename Key>
    void build(std::vector<Key> const& keys, config const& cfg = {}) {
        build_internal(keys, cfg);
    }

    /* Lookup: returns the position assigned to `key` in [0, num_keys()). */
    template <typename Key>
    uint64_t operator()(Key const& key) const {
        return get(key);
    }

    template <typename Key>
    uint64_t get(Key const& key) const {
        uint64_t h0 = Hasher::hash_one(key, 0);
        uint16_t s0 = m_seed_size.get(m_level0_seeds.data(), m_level0_core.bucket_for(h0));
        if (s0 != 0) return SeedChooser::f(h0, s0, m_level0_core);
        for (size_t lv = 0; lv < m_levels.size(); ++lv) {
            level const& l = m_levels[lv];
            uint64_t h = Hasher::hash_one(key, static_cast<uint64_t>(lv) + 1);
            uint16_t s = m_seed_size.get(l.seeds.data(), l.core.bucket_for(h));
            if (s != 0) {
                uint64_t pos = SeedChooser::f(h, s, l.core);
                return m_unassigned.get(pos + l.shift);
            }
        }
        // Unreachable for keys from the input set.
        return uint64_t(-1);
    }

    uint64_t num_keys() const { return m_num_keys; }

    /* Output range of the (minimal) function. */
    uint64_t output_range() const {
        return m_level0_core.output_range(SeedChooser::extra_shift(SeedSize::bits_per_seed));
    }

    /* Size of the output table. For a minimal perfect hash function this equals num_keys(). */
    uint64_t table_size() const { return m_num_keys; }

    template <typename Visitor>
    void visit(Visitor& visitor) const {
        visit_impl(visitor, *this);
    }
    template <typename Visitor>
    void visit(Visitor& visitor) {
        visit_impl(visitor, *this);
    }

private:
    struct level {
        generic_core core;
        std::vector<vec_element> seeds;
        uint64_t shift = 0;

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
            visitor.visit(t.core);
            visitor.visit(t.seeds);
            visitor.visit(t.shift);
        }
    };

    /* Configure a generic_core for `n` keys with the chooser-specified slice length. */
    static generic_core configure_core(uint64_t n, config const& cfg) {
        uint64_t output_range = SeedChooser::minimal_output_range(n);
        uint16_t es = SeedChooser::extra_shift(SeedSize::bits_per_seed);
        uint64_t out_no_shift = output_range > es ? output_range - es : 0;
        uint16_t sl =
            SeedChooser::slice_len(out_no_shift, SeedSize::bits_per_seed, cfg.preferred_slice_len);
        return generic_core::make(output_range, n, cfg.bucket_size_100, sl, es);
    }

    template <typename Key>
    void build_internal(std::vector<Key> const& input_keys, config const& cfg) {
        m_num_keys = input_keys.size();
        if (m_num_keys == 0) {
            m_level0_core = generic_core{};
            return;
        }

        // ---- Level 0 ----
        m_level0_core = configure_core(m_num_keys, cfg);
        std::vector<uint64_t> hashes;
        hashes.reserve(m_num_keys);
        for (auto const& k : input_keys) hashes.push_back(Hasher::hash_one(k, 0));
        // Sort ascending: this groups keys by bucket and orders them by slice_begin
        // within each bucket (both quantities are monotone in the hash).
        std::sort(hashes.begin(), hashes.end());
        weights w0 = SeedChooser::bucket_evaluator(SeedSize::bits_per_seed, m_level0_core.slice_len());
        auto lvl0 = build_level_st<SeedSize, SeedChooser>(hashes.data(), hashes.size(),
                                                          m_level0_core, m_seed_size,
                                                          SeedChooser{}, w0);
        m_level0_seeds = std::move(lvl0.seeds);

        // Filter bumped keys for next level.
        std::vector<Key> remaining;
        remaining.reserve(lvl0.unassigned_len);
        for (auto const& k : input_keys) {
            uint64_t h = Hasher::hash_one(k, 0);
            uint16_t s = m_seed_size.get(m_level0_seeds.data(), m_level0_core.bucket_for(h));
            if (s == 0) remaining.push_back(k);
        }

        // Iterator over level 0's unassigned (free) positions, ascending.
        detail::bit_ones_iterator level0_unassigned(lvl0.unassigned_bm.data(),
                                                    lvl0.unassigned_bm.size());

        // ---- Higher levels ----
        std::vector<uint64_t> unassigned;
        unassigned.reserve(lvl0.unassigned_len * 3 / 2);

        m_levels.clear();
        m_levels.reserve(16);

        uint64_t last = 0;
        uint64_t lvl_nr = 1;
        while (!remaining.empty()) {
            size_t prev_count = remaining.size();
            generic_core core_l = configure_core(prev_count, cfg);
            std::vector<uint64_t> hl;
            hl.reserve(prev_count);
            for (auto const& k : remaining) hl.push_back(Hasher::hash_one(k, lvl_nr));
            std::sort(hl.begin(), hl.end());
            weights wl = SeedChooser::bucket_evaluator(SeedSize::bits_per_seed, core_l.slice_len());
            auto lvl = build_level_st<SeedSize, SeedChooser>(hl.data(), hl.size(), core_l,
                                                             m_seed_size, SeedChooser{}, wl);

            uint64_t shift = unassigned.size();
            for (size_t i = 0; i < prev_count; ++i) {
                if (!detail::bit_get(lvl.unassigned_bm, i)) {
                    size_t v;
                    if (!level0_unassigned.next(v)) {
                        throw std::runtime_error(
                            "phast: ran out of level-0 free slots while building levels");
                    }
                    last = v;
                }
                unassigned.push_back(last);
            }

            level new_level;
            new_level.core = core_l;
            new_level.seeds = std::move(lvl.seeds);
            new_level.shift = shift;
            m_levels.push_back(std::move(new_level));

            // Filter bumped keys for next level.
            std::vector<Key> next;
            next.reserve(lvl.unassigned_len);
            for (auto const& k : remaining) {
                uint64_t h = Hasher::hash_one(k, lvl_nr);
                uint16_t s = m_seed_size.get(m_levels.back().seeds.data(), core_l.bucket_for(h));
                if (s == 0) next.push_back(k);
            }
            remaining = std::move(next);
            ++lvl_nr;
        }

        // Build the compressed array over the full unassigned buffer.
        if (!unassigned.empty()) {
            m_unassigned.build(unassigned, last, m_num_keys);
        } else {
            m_unassigned = CompressedArray{};
        }
    }

    template <typename Visitor, typename T>
    static void visit_impl(Visitor& visitor, T&& t) {
        visitor.visit(t.m_num_keys);
        visitor.visit(t.m_level0_core);
        visitor.visit(t.m_level0_seeds);
        visitor.visit(t.m_levels);
        visitor.visit(t.m_unassigned);
    }

    SeedSize m_seed_size;
    uint64_t m_num_keys = 0;
    generic_core m_level0_core;
    std::vector<vec_element> m_level0_seeds;
    std::vector<level> m_levels;
    CompressedArray m_unassigned;
};

}  // namespace phast
