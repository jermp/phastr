/*
    A small end-to-end demo: builds a PHastR minimal perfect hash function
    over n random 64-bit keys, verifies that it is minimal and perfect, and
    prints its size in bits per key.

    Usage: ./example [n] [num_threads]
*/

#include <chrono>
#include <iostream>
#include <random>
#include <vector>

#include "phastr/phastr.hpp"

int main(int argc, char** argv) {
    uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
    uint64_t num_threads = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;

    std::cout << "generating " << n << " random 64-bit keys..." << std::endl;
    std::mt19937_64 rng(1);
    std::vector<uint64_t> keys(static_cast<size_t>(n * 1.05));
    std::generate(keys.begin(), keys.end(), [&]() { return rng(); });
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    keys.resize(n);
    std::shuffle(keys.begin(), keys.end(), rng);

    phastr::build_configuration config;
    config.num_threads = num_threads;

    phastr::default_function f;
    auto t0 = std::chrono::high_resolution_clock::now();
    f.build_in_internal_memory(keys.begin(), keys.size(), config);
    auto t1 = std::chrono::high_resolution_clock::now();
    const double s = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "built in " << s << " s (" << (n / s) / 1000000.0 << " M keys/s)" << std::endl;

    std::vector<bool> seen(n, false);
    for (auto key : keys) {
        const uint64_t v = f(key);
        if (v >= n || seen[v]) {
            std::cerr << "ERROR: not a minimal perfect hash function" << std::endl;
            return 1;
        }
        seen[v] = true;
    }
    std::cout << "function verified: minimal perfect on " << n << " keys" << std::endl;
    std::cout << "size = " << f.num_bits() / 8 << " bytes ("
              << static_cast<double>(f.num_bits()) / n << " bits / key)" << std::endl;
    return 0;
}
