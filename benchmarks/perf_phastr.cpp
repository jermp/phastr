/*
    PHastR performance benchmark.

    Measures, for a given number of random 64-bit keys:
      - construction time (and throughput), in internal or external memory,
        with a given number of threads
      - lookup latency (sequential and shuffled)
      - serialized size (bits per key) and load time

    Usage:
        ./perf_phastr [num_keys] [num_lookups] [num_runs] [seed] [num_threads] [external]

    Defaults: 10M keys, 10M lookups, 3 runs, seed = 1, 1 thread, internal
    memory (pass 1 as the sixth argument for external memory).
*/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "essentials.hpp"
#include "phastr/bench_util.hpp"
#include "phastr/phastr.hpp"

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
        uint64_t v = keys.back() + 1;
        while (keys.size() < n) keys.push_back(v++);
    }
    keys.resize(n);
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

static void run_one(uint64_t num_keys, uint64_t num_lookups, uint64_t seed, uint64_t num_threads,
                    bool external, int run_index, int total_runs)  //
{
    std::cout << "\n[run " << run_index << "/" << total_runs << "] " << fmt_num(num_keys)
              << " keys, " << fmt_num(num_lookups) << " lookups, " << num_threads << " thread(s), "
              << (external ? "external" : "internal") << " memory\n";
    std::cout << "----------------------------------------\n";

    std::cout << "generating " << fmt_num(num_keys) << " distinct uint64_t keys ... " << std::flush;
    auto t0 = clk::now();
    auto keys = distinct_uint64(num_keys, seed);
    auto t1 = clk::now();
    std::cout << seconds(t0, t1) << " s\n";

    phastr::build_configuration config;
    config.num_threads = num_threads;

    // ---- Build ----
    phastr::default_function f;
    auto b0 = clk::now();
    if (external) {
        f.build_in_external_memory(keys.begin(), keys.size(), config);
    } else {
        f.build_in_internal_memory(keys.begin(), keys.size(), config);
    }
    auto b1 = clk::now();
    const double build_s = seconds(b0, b1);
    std::cout << "build:        " << build_s << " s  (" << (num_keys / build_s) / 1000000.0
              << " M keys/s, " << (build_s * 1e9) / num_keys << " ns/key)\n";

    // ---- Check ----
    if (!phastr::check(keys.begin(), f)) std::exit(1);

    // ---- Size ----
    const uint64_t bytes = f.num_bits() / 8;
    std::cout << "size:         " << fmt_num(bytes) << " bytes  ("
              << static_cast<double>(f.num_bits()) / num_keys << " bits/key)\n";

    // ---- Bumped keys ----
    uint64_t bumped = 0;
    for (auto key : keys) bumped += f.is_bumped(key);
    std::cout << "bumped:       " << fmt_num(bumped) << " keys  (" << (100.0 * bumped) / num_keys
              << "%)\n";

    // ---- Lookups ----
    const double lookup_ns = phastr::perf(keys.begin(), num_lookups, f);
    std::cout << "lookup:       " << lookup_ns << " ns/key  (" << 1000.0 / lookup_ns
              << " M lookups/s)\n";

    // ---- Save / load ----
    const std::string path = "tmp_perf_phastr.bin";
    auto s0 = clk::now();
    essentials::save(f, path.c_str());
    auto s1 = clk::now();
    std::cout << "save:         " << seconds(s0, s1) << " s  ("
              << essentials::file_size(path.c_str()) << " B)\n";
    phastr::default_function g;
    auto l0 = clk::now();
    essentials::load(g, path.c_str());
    auto l1 = clk::now();
    std::cout << "load:         " << seconds(l0, l1) << " s\n";
    std::remove(path.c_str());
}

int main(int argc, char** argv) {
    uint64_t num_keys = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10000000;
    uint64_t num_lookups = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 10000000;
    int num_runs = argc > 3 ? std::atoi(argv[3]) : 3;
    uint64_t seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1;
    uint64_t num_threads = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 1;
    bool external = argc > 6 ? std::atoi(argv[6]) != 0 : false;

    std::cout << "PHastR benchmark\n";
    std::cout << "  num_keys    = " << fmt_num(num_keys) << "\n";
    std::cout << "  num_lookups = " << fmt_num(num_lookups) << "\n";
    std::cout << "  num_runs    = " << num_runs << "\n";
    std::cout << "  seed        = " << seed << "\n";
    std::cout << "  num_threads = " << num_threads << "\n";
    std::cout << "  external    = " << (external ? "yes" : "no") << "\n";

    for (int run = 1; run <= num_runs; ++run) {
        run_one(num_keys, num_lookups, seed + static_cast<uint64_t>(run) - 1, num_threads, external,
                run, num_runs);
    }
    return 0;
}
