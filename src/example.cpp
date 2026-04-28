#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <vector>

#include "phast.hpp"

int main(int argc, char** argv) {
    uint64_t n = (argc > 1) ? std::stoull(argv[1]) : 1'000'000ULL;

    std::cout << "generating " << n << " random 64-bit keys...\n";
    std::mt19937_64 rng(42);
    std::set<uint64_t> uniq;
    while (uniq.size() < n) uniq.insert(rng());
    std::vector<uint64_t> keys(uniq.begin(), uniq.end());

    using clock = std::chrono::high_resolution_clock;
    auto t0 = clock::now();

    phast::default_function f;
    f.build(keys);

    auto t1 = clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "built in " << sec << " s (" << (n / sec / 1e6) << " M keys/s)\n";

    std::set<uint64_t> seen;
    for (auto k : keys) {
        uint64_t v = f(k);
        if (v >= n || !seen.insert(v).second) {
            std::cerr << "ERROR: function is not minimal/perfect\n";
            return 1;
        }
    }
    std::cout << "function verified: minimal perfect on " << n << " keys\n";

    essentials::sizer sizer;
    sizer.visit(f);
    std::cout << "size = " << sizer.bytes() << " bytes (" << (sizer.bytes() * 8.0 / n)
              << " bits / key)\n";
    return 0;
}
