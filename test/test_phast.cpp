#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "phast.hpp"

static int failed = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            ++failed;                                                                      \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n";  \
        }                                                                                  \
    } while (0)

template <typename Function, typename Keys>
static void check_mphf(Function const& f, Keys const& keys) {
    std::set<uint64_t> seen;
    for (auto const& k : keys) {
        uint64_t v = f(k);
        CHECK(v < keys.size());
        CHECK(seen.insert(v).second);
    }
    CHECK(seen.size() == keys.size());
}

static void test_small_uint64() {
    std::vector<uint64_t> keys = {1, 2, 3, 4, 5};
    phast::default_function f;
    f.build(keys);
    check_mphf(f, keys);
}

static void test_medium_uint64() {
    std::vector<uint64_t> keys;
    for (uint64_t i = 0; i < 100; ++i) keys.push_back(i * 7919 + 13);
    phast::default_function f;
    f.build(keys);
    check_mphf(f, keys);
}

static void test_random_uint64(size_t n, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::set<uint64_t> uniq;
    while (uniq.size() < n) uniq.insert(rng());
    std::vector<uint64_t> keys(uniq.begin(), uniq.end());
    std::shuffle(keys.begin(), keys.end(), rng);
    phast::default_function f;
    f.build(keys);
    check_mphf(f, keys);
}

static void test_strings() {
    std::vector<std::string> keys;
    for (int i = 0; i < 200; ++i) keys.push_back("key_" + std::to_string(i * 31 + 7));
    phast::default_function f;
    f.build(keys);
    check_mphf(f, keys);
}

static void test_save_load(size_t n) {
    std::mt19937_64 rng(0xc0ffee);
    std::set<uint64_t> uniq;
    while (uniq.size() < n) uniq.insert(rng());
    std::vector<uint64_t> keys(uniq.begin(), uniq.end());

    phast::default_function f;
    f.build(keys);

    char const* fn = "phast_test.bin";
    essentials::save(f, fn);

    phast::default_function g;
    essentials::load(g, fn);

    check_mphf(g, keys);
    for (auto const& k : keys) CHECK(f(k) == g(k));

    std::remove(fn);
}

static void test_compact_array() {
    using fn_t =
        phast::function<phast::default_hasher, phast::bits8_seed_size, phast::seed_only,
                        phast::compact_array>;
    std::vector<uint64_t> keys;
    for (uint64_t i = 0; i < 500; ++i) keys.push_back(i * 1009 + 17);
    fn_t f;
    f.build(keys);
    check_mphf(f, keys);
}

int main() {
    std::cout << "test_small_uint64\n";
    test_small_uint64();
    std::cout << "test_medium_uint64\n";
    test_medium_uint64();
    std::cout << "test_random_uint64 (1000)\n";
    test_random_uint64(1000, 42);
    std::cout << "test_random_uint64 (10000)\n";
    test_random_uint64(10000, 7);
    std::cout << "test_random_uint64 (100000)\n";
    test_random_uint64(100000, 99);
    std::cout << "test_random_uint64 (1000000)\n";
    test_random_uint64(1000000, 1234);
    std::cout << "test_strings\n";
    test_strings();
    std::cout << "test_save_load\n";
    test_save_load(2000);
    std::cout << "test_compact_array\n";
    test_compact_array();
    if (failed == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }
    std::cout << failed << " tests failed\n";
    return 1;
}
