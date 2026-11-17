/*
    Interleaved lookup duel between PHast and PHastR: both functions are
    built on the same keys, and their lookup times are measured in
    alternating rounds, so that thermal and frequency effects, cache state,
    and measurement order affect both equally; medians over the rounds are
    reported.

    PHast and PHastR use the very same hasher (checked at startup), so
    lookup times are directly comparable.

    Usage:
        ./compare_lookup [num_keys] [num_lookups] [num_rounds] [seed] [num_threads]

    Defaults: 100M keys, 10M lookups, 7 rounds, seed = 0, 4 threads (used
    only by the PHastR construction; PHast builds with one thread).
*/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include "essentials.hpp"
#include "phast.hpp"
#include "phastr/phastr.hpp"

using clk = std::chrono::high_resolution_clock;

template <typename Function>
static double measure(Function const& f, std::vector<uint64_t> const& queries) {
    uint64_t sum = 0;
    auto t0 = clk::now();
    for (auto q : queries) sum += f(q);
    auto t1 = clk::now();
    essentials::do_not_optimize_away(sum);
    return std::chrono::duration<double, std::nano>(t1 - t0).count() /
           static_cast<double>(queries.size());
}

static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

int main(int argc, char** argv) {
    uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000000;
    uint64_t num_lookups = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 10000000;
    int rounds = argc > 3 ? std::atoi(argv[3]) : 7;
    uint64_t seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 0;
    uint64_t num_threads = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 4;

    /* PHast and PHastR must use the very same hasher. */
    for (uint64_t key : {uint64_t(0), uint64_t(42), uint64_t(-1)}) {
        for (uint64_t s : {uint64_t(0), uint64_t(1), seed}) {
            if (phast::default_hasher::hash_one(key, s) != phastr::default_hasher(s)(key)) {
                std::cerr << "ERROR: PHast and PHastR hashers differ" << std::endl;
                return 1;
            }
        }
    }

    std::cout << "generating " << n << " distinct uint64_t keys ..." << std::endl;
    std::mt19937_64 rng(seed + 1);
    std::vector<uint64_t> keys(static_cast<size_t>(n * 1.05));
    std::generate(keys.begin(), keys.end(), [&]() { return rng(); });
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    keys.resize(n);
    std::shuffle(keys.begin(), keys.end(), rng);
    std::vector<uint64_t> queries(keys.begin(), keys.begin() + std::min<uint64_t>(num_lookups, n));

    std::cout << "building PHastR (lambda=4.25, " << num_threads << " threads) ..." << std::endl;
    phastr::build_configuration config;
    config.seed = seed;
    config.num_threads = num_threads;
    phastr::default_function phastr_f;
    phastr_f.build_in_internal_memory(keys.begin(), keys.size(), config);

    std::cout << "building PHast (default, 1 thread) ..." << std::endl;
    phast::default_function phast_f;
    phast_f.build(keys);

    uint64_t bumped = 0;
    for (auto key : keys) bumped += phastr_f.is_bumped(key);
    std::cout << "PHastR bumped: " << (100.0 * bumped) / n << "%" << std::endl;

    std::vector<double> t_phast, t_phastr;
    for (int round = 0; round != rounds; ++round) {
        t_phast.push_back(measure(phast_f, queries));
        t_phastr.push_back(measure(phastr_f, queries));
        std::cout << "round " << round << ": PHast " << t_phast.back() << "  PHastR "
                  << t_phastr.back() << std::endl;
    }
    std::cout << "\nmedians over " << rounds << " interleaved rounds (" << n << " keys):\n";
    std::cout << "  PHast:  " << median(t_phast) << " ns/key\n";
    std::cout << "  PHastR: " << median(t_phastr) << " ns/key" << std::endl;
    return 0;
}
