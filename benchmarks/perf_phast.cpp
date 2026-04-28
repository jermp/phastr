/*
    PHast performance benchmark.

    Measures, for a given number of random 64-bit keys:
      - construction time (and throughput)
      - lookup latency (sequential and shuffled)
      - serialized size (bits per key) and load time

    Usage:
        ./perf_phast [num_keys] [num_lookups] [num_runs] [seed]

    Defaults: 10M keys, 10M lookups, 3 runs, seed = 1.
*/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "essentials.hpp"
#include "phast.hpp"

using clk = std::chrono::high_resolution_clock;

static inline double seconds(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

template <typename T>
static std::string fmt_num(T x) {
    std::ostringstream os;
    os.imbue(std::locale(""));
    os << std::fixed << x;
    return os.str();
}

/* Generate `n` distinct uint64_t keys, slightly oversampled then deduplicated. */
static std::vector<uint64_t> distinct_uint64(uint64_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> keys(static_cast<size_t>(n * 1.05));
    std::generate(keys.begin(), keys.end(), rng);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.size() < n) {
        // unlikely with 64-bit values; fall back to sequential fill
        uint64_t v = keys.back() + 1;
        while (keys.size() < n) keys.push_back(v++);
    }
    keys.resize(n);
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

template <typename Function>
static void run_one(uint64_t num_keys, uint64_t num_lookups, uint64_t seed,
                    int run_index, int total_runs)  //
{
    std::cout << "\n[run " << run_index << "/" << total_runs << "] " << fmt_num(num_keys)
              << " keys, " << fmt_num(num_lookups) << " lookups\n";
    std::cout << "----------------------------------------\n";

    std::cout << "generating " << fmt_num(num_keys) << " distinct uint64_t keys ... "
              << std::flush;
    auto t0 = clk::now();
    auto keys = distinct_uint64(num_keys, seed);
    auto t1 = clk::now();
    std::cout << seconds(t0, t1) << " s\n";

    // ---- Build ----
    Function f;
    auto b0 = clk::now();
    f.build(keys);
    auto b1 = clk::now();
    double build_s = seconds(b0, b1);
    std::cout << "build:        " << build_s << " s  ("
              << (num_keys / build_s / 1e6) << " M keys/s)\n";

    // ---- Verify minimality ----
    if (!phast::check(keys.begin(), f)) {
        std::cerr << "ERROR: function is not minimal/perfect\n";
        std::exit(1);
    }

    // ---- Size ----
    essentials::sizer sizer;
    sizer.visit(f);
    double bits_per_key = sizer.bytes() * 8.0 / num_keys;
    std::cout << "size:         " << sizer.bytes() << " bytes  ("
              << bits_per_key << " bits/key)\n";

    // ---- Lookup throughput ----
    // Use a freshly shuffled subset of size num_lookups. Reusing input keys is fine
    // since perfect hashing is most often queried for known keys.
    std::vector<uint64_t> queries;
    queries.reserve(num_lookups);
    {
        std::mt19937_64 rng(seed ^ 0xdeadbeefULL);
        std::vector<uint64_t> shuffled(keys);
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        if (num_lookups <= shuffled.size()) {
            queries.assign(shuffled.begin(), shuffled.begin() + num_lookups);
        } else {
            queries = shuffled;
            while (queries.size() < num_lookups) {
                queries.insert(queries.end(), shuffled.begin(),
                               shuffled.begin() +
                                   std::min<size_t>(num_lookups - queries.size(),
                                                    shuffled.size()));
            }
        }
    }
    {
        double nanosec_per_key = phast::perf(queries.begin(), num_lookups, f);
        std::cout << "lookup:       " << nanosec_per_key << " ns/key  ("
                  << (1e3 / nanosec_per_key) << " M lookups/s)\n";
    }

    // ---- Serialization round-trip ----
    {
        char const* fn = "perf_phast.bin";
        auto s0 = clk::now();
        size_t saved_bytes = essentials::save(f, fn);
        auto s1 = clk::now();

        Function g;
        auto l0 = clk::now();
        size_t loaded_bytes = essentials::load(g, fn);
        auto l1 = clk::now();
        std::remove(fn);

        std::cout << "save:         " << seconds(s0, s1) << " s  (" << saved_bytes << " B)\n";
        std::cout << "load:         " << seconds(l0, l1) << " s  (" << loaded_bytes << " B)\n";

        // Spot check the loaded function on a small sample.
        for (size_t i = 0; i < std::min<size_t>(1024, keys.size()); ++i) {
            if (g(keys[i]) != f(keys[i])) {
                std::cerr << "ERROR: load returned a different function\n";
                std::exit(1);
            }
        }
    }
}

int main(int argc, char** argv) {
    uint64_t num_keys = 10'000'000ULL;
    uint64_t num_lookups = 10'000'000ULL;
    int num_runs = 3;
    uint64_t seed = 1;

    if (argc >= 2) num_keys = std::stoull(argv[1]);
    if (argc >= 3) num_lookups = std::stoull(argv[2]);
    if (argc >= 4) num_runs = std::stoi(argv[3]);
    if (argc >= 5) seed = std::stoull(argv[4]);

    std::cout << "PHast performance benchmark\n";
    std::cout << "  num_keys    = " << fmt_num(num_keys) << "\n";
    std::cout << "  num_lookups = " << fmt_num(num_lookups) << "\n";
    std::cout << "  num_runs    = " << num_runs << "\n";
    std::cout << "  seed        = " << seed << "\n";

    using fn_t = phast::default_function;
    for (int r = 1; r <= num_runs; ++r) {
        run_one<fn_t>(num_keys, num_lookups, seed + r, r, num_runs);
    }
    return 0;
}
