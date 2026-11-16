#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "phastr/phastr.hpp"
#include "essentials.hpp"

static int failed = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            ++failed;                                                                     \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n"; \
        }                                                                                 \
    } while (0)

/* Checks that f is a minimal perfect hash function on the keys. */
template <typename Function, typename Keys>
static bool is_mphf(Function const& f, Keys const& keys) {
    if (f.num_keys() != keys.size()) return false;
    std::vector<bool> seen(keys.size(), false);
    for (auto const& key : keys) {
        const uint64_t v = f(key);
        if (v >= keys.size() || seen[v]) return false;
        seen[v] = true;
    }
    return true;
}

/* Serializes f to a buffer, for structural comparisons. */
template <typename Function>
static std::string serialize(Function const& f) {
    const std::string path = "tmp_test_phastr.bin";
    essentials::save(const_cast<Function&>(f), path.c_str());
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::remove(path.c_str());
    return ss.str();
}

static std::vector<uint64_t> distinct_keys(size_t n, unsigned seed = 42) {
    std::mt19937_64 rng(seed);
    std::set<uint64_t> uniq;
    while (uniq.size() < n) uniq.insert(rng());
    std::vector<uint64_t> keys(uniq.begin(), uniq.end());
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

/* Builds in internal and external memory, checking that the functions are
   identical and minimal perfect. */
template <typename Function = phastr::default_function>
static void check_build(std::vector<uint64_t> const& keys, phastr::build_configuration config) {
    Function internal;
    internal.build_in_internal_memory(keys.begin(), keys.size(), config);
    CHECK(is_mphf(internal, keys));
    Function external;
    external.build_in_external_memory(keys.begin(), keys.size(), config);
    CHECK(serialize(internal) == serialize(external));
}

static void test_sizes() {
    for (size_t n : {0, 1, 2, 3, 10, 100, 1000, 4096, 4097, 100000, 300000}) {
        std::cout << "test_sizes " << n << std::endl;
        check_build(distinct_keys(n), {});
    }
}

static void test_parameters() {
    phastr::build_configuration config;

    std::cout << "test_parameters: 10-bit seeds" << std::endl;
    config = {};
    config.seed_bits = 10;
    config.log2_slice_len = 11;
    config.bucket_size = 5.75;
    check_build<phastr::function<phastr::default_hasher, phastr::compact_seeds>>(
        distinct_keys(100000), config);

    std::cout << "test_parameters: single layout" << std::endl;
    config = {};
    config.log2_layouts = 0;
    config.log2_slice_len = 8;
    check_build(distinct_keys(100000), config);

    std::cout << "test_parameters: eight layouts" << std::endl;
    config = {};
    config.log2_layouts = 3;
    config.log2_slice_len = 8;
    check_build(distinct_keys(100000), config);

    std::cout << "test_parameters: large buckets" << std::endl;
    config = {};
    config.bucket_size = 64.0;
    check_build(distinct_keys(100000), config);

    std::cout << "test_parameters: few seed bits" << std::endl;
    config = {};
    config.seed_bits = 4;
    config.log2_slice_len = 8;
    config.bucket_size = 2.0;
    check_build(distinct_keys(100000), config);

    std::cout << "test_parameters: short slices" << std::endl;
    config = {};
    config.log2_slice_len = 6;
    check_build(distinct_keys(300000), config);

    std::cout << "test_parameters: nonzero hash seed" << std::endl;
    config = {};
    config.seed = 1234567;
    check_build(distinct_keys(100000), config);
}

static void test_threads() {
    /* With a fixed number of sweep chunks, the function does not depend on
       the number of threads; the key set is large enough for three
       chunks. */
    auto keys = distinct_keys(400000);
    phastr::build_configuration config;
    config.sweep_chunks = 3;
    phastr::default_function reference;
    reference.build_in_internal_memory(keys.begin(), keys.size(), config);
    CHECK(is_mphf(reference, keys));
    const std::string expected = serialize(reference);
    for (uint64_t threads : {1, 2, 3, 4}) {
        std::cout << "test_threads " << threads << std::endl;
        config.num_threads = threads;
        phastr::default_function internal, external;
        internal.build_in_internal_memory(keys.begin(), keys.size(), config);
        external.build_in_external_memory(keys.begin(), keys.size(), config);
        CHECK(serialize(internal) == expected);
        CHECK(serialize(external) == expected);
    }
}

static void test_duplicates() {
    for (size_t n : {1000, 100000}) {
        std::cout << "test_duplicates " << n << std::endl;
        auto keys = distinct_keys(n);
        keys.push_back(keys[n / 2]);
        bool thrown = false;
        try {
            phastr::default_function f;
            f.build_in_internal_memory(keys.begin(), keys.size(), {});
        } catch (std::runtime_error const&) { thrown = true; }
        CHECK(thrown);
        thrown = false;
        try {
            phastr::default_function f;
            f.build_in_external_memory(keys.begin(), keys.size(), {});
        } catch (std::runtime_error const&) { thrown = true; }
        CHECK(thrown);
    }
}

/* A hasher whose signatures with the configured seed collide for pairs of
   keys (2i, 2i + 1) among the first 2 * COLLIDING keys; other seeds give
   independent signatures. Colliding keys are bumped from the first level
   and separated by the second hash at the following levels. */
struct colliding_hasher {
    static constexpr uint64_t COLLIDING = 2000;
    static inline uint64_t hash(uint64_t key, uint64_t seed) {
        if (seed == 0 && key < 2 * COLLIDING) { return phastr::default_hasher::hash(key / 2, 0); }
        return phastr::default_hasher::hash(key, seed);
    }
};

static void test_colliding_signatures() {
    for (size_t n : {5000, 300000}) {
        std::cout << "test_colliding_signatures " << n << std::endl;
        std::vector<uint64_t> keys(n);
        for (size_t i = 0; i != n; ++i) keys[i] = i;
        phastr::function<colliding_hasher> internal, external;
        internal.build_in_internal_memory(keys.begin(), keys.size(), {});
        CHECK(is_mphf(internal, keys));
        external.build_in_external_memory(keys.begin(), keys.size(), {});
        CHECK(serialize(internal) == serialize(external));
    }
}

struct string_hasher {
    static inline uint64_t hash(std::string const& key, uint64_t seed) {
        uint64_t h = phastr::splitmix64(seed) ^ 0x9e3779b97f4a7c15ULL;
        for (char c : key) {
            h ^= static_cast<uint8_t>(c);
            h *= 0x100000001b3ULL;
        }
        return phastr::splitmix64(h);
    }
};

static void test_strings() {
    std::cout << "test_strings" << std::endl;
    std::vector<std::string> keys;
    for (int i = 0; i < 200000; ++i) keys.push_back("key_" + std::to_string(i));
    phastr::function<string_hasher> f;
    f.build_in_internal_memory(keys.begin(), keys.size(), {});
    CHECK(is_mphf(f, keys));
}

static void test_save_load() {
    std::cout << "test_save_load" << std::endl;
    auto keys = distinct_keys(100000);
    phastr::default_function f;
    f.build_in_internal_memory(keys.begin(), keys.size(), {});
    const std::string path = "tmp_test_phastr_save.bin";
    essentials::save(f, path.c_str());
    phastr::default_function g;
    essentials::load(g, path.c_str());
    std::remove(path.c_str());
    CHECK(g.num_keys() == f.num_keys());
    for (auto key : keys) CHECK(g(key) == f(key));
}

static void test_bumped_fraction() {
    std::cout << "test_bumped_fraction" << std::endl;
    /* With the default parameters, the first level bumps about 0.5% of the
       keys (see the PHastR paper). */
    auto keys = distinct_keys(1000000);
    phastr::default_function f;
    f.build_in_internal_memory(keys.begin(), keys.size(), {});
    uint64_t bumped = 0;
    for (auto key : keys) bumped += f.is_bumped(key);
    const double fraction = static_cast<double>(bumped) / keys.size();
    CHECK(fraction < 0.01);
    /* Space should be around 1.94 bits per key. */
    const double bits_per_key = static_cast<double>(f.num_bits()) / keys.size();
    CHECK(bits_per_key < 2.1);
}

int main() {
    test_sizes();
    test_parameters();
    test_threads();
    test_duplicates();
    test_colliding_signatures();
    test_strings();
    test_save_load();
    test_bumped_fraction();
    if (failed != 0) {
        std::cerr << failed << " checks failed" << std::endl;
        return 1;
    }
    std::cout << "all tests passed" << std::endl;
    return 0;
}
