#pragma once

/*
    Correctness check and lookup-performance benchmark, kept identical to
    PTHash's `pthash::check` and `pthash::perf` (see
    https://github.com/jermp/pthash/blob/master/src/util.hpp) so that
    measurements collected with PHast and PTHash are directly comparable.
*/

#include <chrono>
#include <cstdint>
#include <iostream>

#include "bit_vector.hpp"
#include "essentials.hpp"

namespace phast {

template <typename Function, typename Iterator>
bool check(Iterator keys, Function const& f) {
    __uint128_t n = f.num_keys();
    if (Function::minimal) {
        __uint128_t sum = 0;
        for (uint64_t i = 0; i != n; ++i) {
            auto const& key = *keys;
            uint64_t p = f(key);
            if (p >= n) {
                std::cout << "ERROR: position is out of range" << std::endl;
                return false;
            }
            sum += p;
            ++keys;
        }
        if (sum != (n * (n - 1)) / 2) {
            std::cout << "ERROR: mphf contains duplicates" << std::endl;
            return false;
        }
    } else {
        uint64_t m = f.table_size();
        bits::bit_vector::builder taken(m);
        for (uint64_t i = 0; i != n; ++i) {
            auto const& key = *keys;
            uint64_t p = f(key);
            if (p >= m) {
                std::cout << "ERROR: position is out of range" << std::endl;
                return false;
            }
            if (taken.get(p) != 0) {
                std::cout << "ERROR: mphf contains duplicates" << std::endl;
                return false;
            }
            taken.set(p, 1);
            ++keys;
        }
    }
    return true;
}

template <typename Function, typename Iterator>
double perf(Iterator keys, const uint64_t num_queries, Function const& f) {
    static const uint64_t runs = 5;
    essentials::timer<std::chrono::high_resolution_clock, std::chrono::nanoseconds> t;
    t.start();
    for (uint64_t r = 0; r != runs; ++r) {
        Iterator begin = keys;
        for (uint64_t i = 0; i != num_queries; ++i) {
            auto const& key = *begin;
            uint64_t p = f(key);
            essentials::do_not_optimize_away(p);
            ++begin;
        }
    }
    t.stop();
    double nanosec_per_key = t.elapsed() / static_cast<double>(runs * num_queries);
    return nanosec_per_key;
}

}  // namespace phast
