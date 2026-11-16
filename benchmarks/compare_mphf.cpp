/*
    Comparison of minimal perfect hash functions on random 64-bit keys:

      - PTHash, in the two configurations used by SSHash
        (https://github.com/jermp/sshash):
          * the "minimizers" configuration: partitioned PHF, opt_bucketer,
            compact encoder, minimal, lambda = 5.0, alpha = 0.94,
            avg_partition_size = 3000000, built in external memory;
          * the "skew index" configuration: the same type with
            lambda = 7.0, built in internal memory;
        keys are hashed with CityHash128WithSeed, as in SSHash.

      - PHast (Beling and Sanders), default configuration.

      - PHastR (Vigna), with the default parameters (8-bit seeds, four
        layouts, slices of length 1024, lambda = 4.25) and with
        lambda = 4.5, in internal and external memory.

    PHast and PHastR use the very same hasher, the fastest available for
    64-bit keys (an inlined SplitMix64-based mix), so their lookup times
    are directly comparable; PTHash keeps CityHash128WithSeed, as that is
    the configuration used by SSHash.

    For each structure the benchmark reports construction time, space, and
    lookup time on a random (shuffled) stream of keys of the set, plus the
    fraction of keys bumped from the first level for PHastR.

    Usage:
        ./compare_mphf [num_keys] [num_lookups] [num_runs] [seed] [num_threads]

    Defaults: 10M keys, 10M lookups, 3 runs, seed = 1, 4 threads.
*/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "essentials.hpp"

#include "pthash.hpp"

#include "phast.hpp"
#include "phastr/phastr.hpp"

#include "comparison/cityhash.hpp"

using clk = std::chrono::high_resolution_clock;

static inline double seconds(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

/* The hasher used by SSHash for both minimizers and k-mers (here on 64-bit
   keys). */
struct city_hasher_128 {
    typedef pthash::hash128 hash_type;
    static inline pthash::hash128 hash(uint64_t key, uint64_t seed) {
        auto ret = cityhash::CityHash128WithSeed(reinterpret_cast<char const*>(&key), sizeof(key),
                                                 {seed, ~seed});
        return {ret.first, ret.second};
    }
};

/* The PTHash type used by SSHash for the MPHFs of both the minimizers and
   the skew index. */
using sshash_pthash_type = pthash::partitioned_phf<city_hasher_128,       //
                                                   pthash::opt_bucketer,  //
                                                   pthash::compact,       //
                                                   true                   //
                                                   >;

/* PHast and PHastR use the very same hasher: phast::default_hasher and
   phastr::default_hasher both compute splitmix64(key + splitmix64(seed)),
   the fastest hashing scheme available here for 64-bit keys (checked at
   startup in main). */
using phastr_function = phastr::default_function;

struct row {
    std::string name;
    double build_ns = 0;
    double bits_per_key = 0;
    double lookup_ns = 0;
    double bumped_percent = -1.0;
};

static std::vector<row> rows;

static std::vector<uint64_t> distinct_uint64(uint64_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> keys(static_cast<size_t>(n * 1.05));
    std::generate(keys.begin(), keys.end(), rng);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.size() < n) {
        uint64_t v = keys.back() + 1;
        while (keys.size() < n) keys.push_back(v++);
    }
    keys.resize(n);
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

/* Lookup time on a shuffled stream of keys of the set (the keys are already
   shuffled). */
template <typename Function>
static double lookup_ns(Function const& f, std::vector<uint64_t> const& keys,
                        uint64_t num_lookups) {
    uint64_t sum = 0;
    const uint64_t n = keys.size();
    auto t0 = clk::now();
    for (uint64_t i = 0; i != num_lookups; ++i) sum += f(keys[i % n]);
    auto t1 = clk::now();
    essentials::do_not_optimize_away(sum);
    return seconds(t0, t1) * 1e9 / static_cast<double>(num_lookups);
}

template <typename Function, typename = void>
struct has_num_bits : std::false_type {};
template <typename Function>
struct has_num_bits<Function, std::void_t<decltype(std::declval<Function const&>().num_bits())>>
    : std::true_type {};

/* The size of a function, in bits: num_bits() if available, the serialized
   size otherwise (e.g., for phast::function). */
template <typename Function>
static double size_in_bits(Function const& f) {
    if constexpr (has_num_bits<Function>::value) {
        return static_cast<double>(f.num_bits());
    } else {
        const std::string path = "tmp_compare_mphf.bin";
        essentials::save(const_cast<Function&>(f), path.c_str());
        const double bits = 8.0 * static_cast<double>(essentials::file_size(path.c_str()));
        std::remove(path.c_str());
        return bits;
    }
}

template <typename Function>
static bool check_mphf(Function const& f, std::vector<uint64_t> const& keys) {
    const uint64_t n = keys.size();
    std::vector<bool> seen(n, false);
    for (auto key : keys) {
        const uint64_t v = f(key);
        if (v >= n || seen[v]) return false;
        seen[v] = true;
    }
    return true;
}

template <typename Build, typename Function>
static void run_competitor(std::string const& name, Function& f, Build const& build,
                           std::vector<uint64_t> const& keys, uint64_t num_lookups,
                           double bumped_percent = -1.0) {
    std::cout << "\n==> " << name << std::endl;
    auto t0 = clk::now();
    build(f);
    auto t1 = clk::now();
    row r;
    r.name = name;
    r.build_ns = seconds(t0, t1) * 1e9 / static_cast<double>(keys.size());
    r.bits_per_key = size_in_bits(f) / static_cast<double>(keys.size());
    if (!check_mphf(f, keys)) {
        std::cerr << "ERROR: " << name << " is not a minimal perfect hash function" << std::endl;
        std::exit(1);
    }
    r.lookup_ns = lookup_ns(f, keys, num_lookups);
    r.bumped_percent = bumped_percent;
    std::cout << "    build:  " << r.build_ns << " ns/key" << std::endl;
    std::cout << "    space:  " << r.bits_per_key << " bits/key" << std::endl;
    std::cout << "    lookup: " << r.lookup_ns << " ns/key" << std::endl;
    rows.push_back(r);
}

template <typename F>
static double percent_bumped(F const& f, std::vector<uint64_t> const& keys) {
    uint64_t bumped = 0;
    for (auto key : keys) bumped += f.is_bumped(key);
    return 100.0 * static_cast<double>(bumped) / static_cast<double>(keys.size());
}

static void run_all(uint64_t num_keys, uint64_t num_lookups, uint64_t seed, uint64_t num_threads) {
    std::cout << "generating " << num_keys << " distinct uint64_t keys ..." << std::endl;
    auto keys = distinct_uint64(num_keys, seed);

    /* PTHash as used by SSHash for the minimizers MPHF. */
    pthash::build_configuration pt_config;
    pt_config.lambda = 5.0;
    pt_config.alpha = 0.94;
    pt_config.seed = seed;
    pt_config.verbose = false;
    pt_config.num_threads = num_threads;
    pt_config.avg_partition_size = 3000000;
    pt_config.tmp_dir = ".";

    {
        sshash_pthash_type f;
        auto config = pt_config;
        run_competitor(
            "PTHash (sshash minimizers config: lambda=5.0, alpha=0.94, external)", f,
            [&](sshash_pthash_type& f) {
                f.build_in_external_memory(keys.begin(), keys.size(), config);
            },
            keys, num_lookups);
    }
    {
        sshash_pthash_type f;
        auto config = pt_config;
        run_competitor(
            "PTHash (sshash minimizers config: lambda=5.0, alpha=0.94, internal)", f,
            [&](sshash_pthash_type& f) {
                f.build_in_internal_memory(keys.begin(), keys.size(), config);
            },
            keys, num_lookups);
    }
    {
        sshash_pthash_type f;
        auto config = pt_config;
        config.lambda = 7.0; /* lambda + 2.0, as in the skew index */
        run_competitor(
            "PTHash (sshash skew-index config: lambda=7.0, alpha=0.94, internal)", f,
            [&](sshash_pthash_type& f) {
                f.build_in_internal_memory(keys.begin(), keys.size(), config);
            },
            keys, num_lookups);
    }

    /* PHast, default configuration (single-threaded construction). */
    {
        phast::default_function f;
        run_competitor(
            "PHast (default, 1 thread)", f, [&](phast::default_function& f) { f.build(keys); },
            keys, num_lookups);
    }

    /* PHastR. */
    phastr::build_configuration pr_config;
    pr_config.seed = seed;
    pr_config.num_threads = num_threads;
    {
        phastr_function f;
        auto config = pr_config;
        run_competitor(
            "PHastR (lambda=4.25, internal)", f,
            [&](phastr_function& f) { f.build_in_internal_memory(keys.begin(), keys.size(), config); },
            keys, num_lookups);
        rows.back().bumped_percent = percent_bumped(f, keys);
    }
    {
        phastr_function f;
        auto config = pr_config;
        run_competitor(
            "PHastR (lambda=4.25, external)", f,
            [&](phastr_function& f) { f.build_in_external_memory(keys.begin(), keys.size(), config); },
            keys, num_lookups);
        rows.back().bumped_percent = percent_bumped(f, keys);
    }
    {
        phastr_function f;
        auto config = pr_config;
        config.bucket_size = 4.5;
        run_competitor(
            "PHastR (lambda=4.5, internal)", f,
            [&](phastr_function& f) { f.build_in_internal_memory(keys.begin(), keys.size(), config); },
            keys, num_lookups);
        rows.back().bumped_percent = percent_bumped(f, keys);
    }
}

int main(int argc, char** argv) {
    uint64_t num_keys = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10000000;
    uint64_t num_lookups = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 10000000;
    int num_runs = argc > 3 ? std::atoi(argv[3]) : 3;
    uint64_t seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1;
    uint64_t num_threads = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 4;

    std::cout << "MPHF comparison (PTHash as used in SSHash vs. PHast vs. PHastR)\n";
    std::cout << "  num_keys    = " << num_keys << "\n";
    std::cout << "  num_lookups = " << num_lookups << "\n";
    std::cout << "  num_runs    = " << num_runs << "\n";
    std::cout << "  seed        = " << seed << "\n";
    std::cout << "  num_threads = " << num_threads << std::endl;

    /* PHast and PHastR must use the very same hasher. */
    for (uint64_t key : {uint64_t(0), uint64_t(42), uint64_t(-1)}) {
        for (uint64_t s : {uint64_t(0), uint64_t(1), seed}) {
            if (phast::default_hasher::hash_one(key, s) != phastr::default_hasher::hash(key, s)) {
                std::cerr << "ERROR: PHast and PHastR hashers differ" << std::endl;
                return 1;
            }
        }
    }

    for (int run = 0; run != num_runs; ++run) {
        run_all(num_keys, num_lookups, seed + static_cast<uint64_t>(run), num_threads);
    }

    /* Summary: medians over the runs for each competitor. */
    const size_t competitors = rows.size() / static_cast<size_t>(num_runs);
    std::cout << "\n== summary (median of " << num_runs << " runs, " << num_keys << " keys, "
              << num_threads << " threads) ==\n\n";
    std::cout << "| method | build [ns/key] | space [bits/key] | lookup [ns/key] | bumped [%] "
                 "|\n";
    std::cout << "|:---|---:|---:|---:|---:|\n";
    auto median = [&](size_t c, auto get) {
        std::vector<double> v;
        for (int run = 0; run != num_runs; ++run) {
            v.push_back(get(rows[static_cast<size_t>(run) * competitors + c]));
        }
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    for (size_t c = 0; c != competitors; ++c) {
        std::cout << "| " << rows[c].name << " | " << std::fixed << std::setprecision(1)
                  << median(c, [](row const& r) { return r.build_ns; }) << " | "
                  << std::setprecision(3) << median(c, [](row const& r) { return r.bits_per_key; })
                  << " | " << std::setprecision(1)
                  << median(c, [](row const& r) { return r.lookup_ns; }) << " | ";
        if (rows[c].bumped_percent >= 0) {
            std::cout << std::setprecision(2)
                      << median(c, [](row const& r) { return r.bumped_percent; });
        } else {
            std::cout << "-";
        }
        std::cout << " |\n";
    }
    std::cout << std::endl;
    return 0;
}
