# PHastR

A header-only C++17 implementation of **PHastR** (minimal perfect hashing
with rotated layouts), a minimal perfect hash function (MPHF) proposed by
Sebastiano Vigna:

> Sebastiano Vigna. *PHastR: Faster Minimal Perfect Hashing with Rotated
> Layouts*, 2026. [arXiv:2610.17833](https://arxiv.org/abs/2610.17833)

The reference Rust implementation lives in
[vigna/sux-rs](https://github.com/vigna/sux-rs).

With the default parameters (8-bit seeds, four layouts, slices of length
1024, and an expected bucket size of 4.25 keys), PHastR uses about 1.94
bits per key, and it is one of the fastest MPHFs available, both to build
and to query, as it bumps very few keys (about 0.5%). The implementation
supports multi-threaded construction and an external-memory mode that
keeps the key records in temporary files and builds the *identical*
structure, scaling to very large key sets.

This implementation is built on top of
[`jermp/bits`](https://github.com/jermp/bits) (compact vectors,
Elias-Fano) and [`jermp/essentials`](https://github.com/jermp/essentials)
(serialization helpers).

## Requirements

- A C++17 compiler (GCC or Clang)
- CMake >= 3.5
- A POSIX threads implementation (pthreads)
- On x86_64 Linux, the build enables `-march=native -mbmi2 -msse4.2` for
  hardware `popcount` and `pdep`

## Getting the source

The two dependencies (`bits` and `essentials`) are git submodules, so clone
recursively:

```bash
git clone --recurse-submodules https://github.com/jermp/phastr.git
cd phastr
```

If you cloned without `--recurse-submodules`, initialize them after the fact:

```bash
git submodule update --init --recursive
```

## Building

A standard out-of-source CMake build:

```bash
mkdir build
cd build
cmake ..
make -j
```

The default build type is `Release`. To build a debug version:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j
```

To build with AddressSanitizer enabled:

```bash
cmake -DPHASTR_USE_SANITIZERS=ON ..
make -j
```

The build produces these executables in the `build/` directory:

- `example`      — a small end-to-end demo (from `src/example.cpp`)
- `perf_phastr`  — a performance benchmark (from `benchmarks/perf_phastr.cpp`)
- `test_phastr`  — the test suite (from `test/test_phastr.cpp`)

## Running the example

The example builds an MPHF over `n` random 64-bit keys, verifies that it is
minimal and perfect, and prints its size in bits per key.

```bash
./example                 # default: 1,000,000 keys, 1 thread
./example 100000000 8     # 100,000,000 keys, 8 threads
```

## Running the benchmark

`perf_phastr` measures construction throughput, lookup latency, serialized
size, and load time over multiple runs, with optional multi-threaded
construction and external-memory mode:

```bash
./perf_phastr                                # 10M keys, 10M lookups, 3 runs
./perf_phastr <num_keys> <num_lookups> <num_runs> <seed> <num_threads> <external>
./perf_phastr 100000000 10000000 1 42 8 0    # 8 threads, internal memory
./perf_phastr 100000000 10000000 1 42 8 1    # 8 threads, external memory
```

## Running the tests

From the `build/` directory:

```bash
ctest --output-on-failure
```

or run the test binary directly:

```bash
./test_phastr
```

## Using PHastR in your own code

PHastR is header-only. Include `phastr/phastr.hpp` and link against the
`PHASTR` CMake target:

```cpp
#include "phastr/phastr.hpp"

std::vector<uint64_t> keys = /* distinct keys */;

phastr::build_configuration config;
config.num_threads = 8;

phastr::default_function f;
f.build_in_internal_memory(keys.begin(), keys.size(), config);
// or, reading the keys once and keeping their records in temporary files:
// f.build_in_external_memory(keys.begin(), keys.size(), config);

uint64_t pos = f(keys[0]);   // pos is in [0, keys.size())
```

The two build methods read the keys just once from a forward iterator and
build *identical* structures; the structure does not depend on the number
of threads (it depends on `config.sweep_chunks`, which defaults to
`config.num_threads`: fix it to make the structure independent of the
number of threads used).

The default configuration uses 8-bit seeds (stored one per byte), four
layouts, slices of length 1024, and an expected bucket size of 4.25 keys,
for about 1.94 bits per key. For 9- to 16-bit seeds use the packed seed
storage, e.g., with 10-bit seeds:

```cpp
phastr::build_configuration config;
config.seed_bits = 10;
config.log2_slice_len = 11;
config.bucket_size = 5.75;

phastr::function<phastr::default_hasher, phastr::compact_seeds> f;
f.build_in_internal_memory(keys.begin(), keys.size(), config);
```

For key types other than `uint64_t`, supply a hasher with a static method
`hash(Key const&, uint64_t seed) -> uint64_t`.

To use this project as a CMake dependency, add it as a subdirectory and link
the interface target:

```cmake
add_subdirectory(phastr)
target_link_libraries(my_target PRIVATE PHASTR)
```

Functions can be saved and reloaded with `essentials::save` / `essentials::load`:

```cpp
essentials::save(f, "mphf.bin");

phastr::default_function g;
essentials::load(g, "mphf.bin");
```
