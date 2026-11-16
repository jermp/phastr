#pragma once

/*
    Storage for the seeds of the levels of a phastr::function.

    byte_seeds stores one seed per byte (at most 8 bits per seed) and
    provides the fastest queries; compact_seeds packs seeds of any width up
    to 16 bits, read with unaligned 64-bit loads.
*/

#include <vector>

#include "essentials.hpp"
#include "compact_vector.hpp"

#include "util.hpp"

namespace phastr {

struct byte_seeds {
    static constexpr uint32_t max_bits = 8;

    std::vector<uint8_t> data;

    void build(std::vector<uint8_t>&& seeds, uint32_t /* seed_bits */) {
        data = std::move(seeds);
    }
    void build(std::vector<uint16_t> const& seeds, uint32_t /* seed_bits */) {
        data.assign(seeds.begin(), seeds.end());
    }

    inline uint64_t get(uint64_t i) const {
        return data[i];
    }

    inline void prefetch_seed(uint64_t i) const {
        prefetch(data.data() + i);
    }

    uint64_t num_bits() const {
        return 8 * (sizeof(uint64_t) + data.size()); /* size + bytes */
    }

    template <typename Visitor>
    void visit(Visitor& visitor) const {
        visitor.visit(data);
    }
    template <typename Visitor>
    void visit(Visitor& visitor) {
        visitor.visit(data);
    }
};

struct compact_seeds {
    static constexpr uint32_t max_bits = 16;

    bits::compact_vector data;

    void build(std::vector<uint8_t>&& seeds, uint32_t seed_bits) {
        if (seeds.empty()) return;
        data.build(seeds.begin(), seeds.size(), seed_bits);
    }
    void build(std::vector<uint16_t> const& seeds, uint32_t seed_bits) {
        if (seeds.empty()) return;
        data.build(seeds.begin(), seeds.size(), seed_bits);
    }

    inline uint64_t get(uint64_t i) const {
        return data.access(i);
    }

    inline void prefetch_seed(uint64_t i) const {
        prefetch(reinterpret_cast<char const*>(data.data().data()) + i * data.width() / 8);
    }

    uint64_t num_bits() const {
        return 8 * data.num_bytes();
    }

    template <typename Visitor>
    void visit(Visitor& visitor) const {
        data.visit(visitor);
    }
    template <typename Visitor>
    void visit(Visitor& visitor) {
        data.visit(visitor);
    }
};

}  // namespace phastr
