#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "essentials.hpp"
#include "compact_vector.hpp"
#include "elias_fano.hpp"

namespace phast {

/*
    A compressed array of monotonically non-decreasing integers indexed by
    [0, n). Backs PHast's "unassigned" sequence: maps a level's output position
    to the corresponding free slot of level 0.

    Two implementations are provided:
      - `compact_array`     : packs each value with `bits_to_store(max)` bits.
                              Wraps `bits::compact_vector`.
      - `elias_fano_array`  : Elias-Fano encoded sorted sequence.
                              Wraps `bits::elias_fano<...>`.

    Both implementations expose:
      - `static constexpr bool max_for_unused = false;`
      - `void build(std::vector<uint64_t> const& values, uint64_t last, uint64_t num_keys);`
      - `uint64_t get(uint64_t i) const;`
      - `template <typename Visitor> void visit(Visitor&) [const];`
*/

struct compact_array {
    static constexpr bool max_for_unused = false;

    bits::compact_vector m_data;

    compact_array() = default;

    void build(std::vector<uint64_t> const& values, uint64_t /*last*/, uint64_t /*num_keys*/) {
        if (values.empty()) return;
        m_data.build(values.begin(), values.size());
    }

    inline uint64_t get(uint64_t i) const {
        return m_data.access(i);
    }

    uint64_t size() const {
        return m_data.size();
    }

    template <typename Visitor>
    void visit(Visitor& visitor) const {
        m_data.visit(visitor);
    }
    template <typename Visitor>
    void visit(Visitor& visitor) {
        m_data.visit(visitor);
    }
};

struct elias_fano_array {
    static constexpr bool max_for_unused = false;

    bits::elias_fano<false, false> m_ef;

    elias_fano_array() = default;

    void build(std::vector<uint64_t> const& values, uint64_t last, uint64_t /*num_keys*/) {
        if (values.empty()) return;
        m_ef.encode(values.begin(), values.size(), last);
    }

    inline uint64_t get(uint64_t i) const {
        return m_ef.access(i);
    }

    uint64_t size() const {
        return m_ef.size();
    }

    template <typename Visitor>
    void visit(Visitor& visitor) const {
        m_ef.visit(visitor);
    }
    template <typename Visitor>
    void visit(Visitor& visitor) {
        m_ef.visit(visitor);
    }
};

/* The default compressed array: Elias-Fano (best size/lookup tradeoff). */
using default_compressed_array = elias_fano_array;

}  // namespace phast
