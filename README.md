# PHast

A C++17 implementation of **PHast** (Perfect Hashing made fast), a minimal
perfect hash function (MPHF) originally proposed by Piotr Beling and Peter
Sanders:

> Piotr Beling, Peter Sanders. *PHast - Perfect Hashing made fast*, 2025.
> [arXiv:2504.17918](https://arxiv.org/abs/2504.17918)

The reference Rust implementation lives at
[beling/bsuccinct-rs](https://github.com/beling/bsuccinct-rs/tree/main/internal/phast).

This repository provides a header-only C++17 translation built on top of
[`jermp/bits`](https://github.com/jermp/bits) (compact vectors, Elias-Fano)
and [`jermp/essentials`](https://github.com/jermp/essentials)
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
git clone --recurse-submodules https://github.com/jermp/phast.git
cd phast
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
cmake -DPHAST_USE_SANITIZERS=ON ..
make -j
```

The build produces three executables in the `build/` directory:

- `example`     — a small end-to-end demo (from `src/example.cpp`)
- `perf_phast`  — a performance benchmark (from `benchmarks/perf_phast.cpp`)
- `test_phast`  — the test suite (from `test/test_phast.cpp`)

## Running the example

The example builds an MPHF over `n` random 64-bit keys, verifies that it is
minimal and perfect, and prints its size in bits per key.

```bash
./example            # default: 1,000,000 keys
./example 5000000    # 5,000,000 keys
```

Sample output:

```
generating 1000000 random 64-bit keys...
built in 0.42 s (2.38 M keys/s)
function verified: minimal perfect on 1000000 keys
size = 268912 bytes (2.15 bits / key)
```

## Running the benchmark

`perf_phast` measures construction throughput, lookup latency, serialized
size, and load time over multiple runs.

```bash
./perf_phast                                # 10M keys, 10M lookups, 3 runs
./perf_phast <num_keys> <num_lookups> <num_runs> <seed>
./perf_phast 50000000 10000000 1 42
```

## Running the tests

From the `build/` directory:

```bash
ctest --output-on-failure
```

or run the test binary directly:

```bash
./test_phast
```

## Using PHast in your own code

PHast is header-only. Include `phast.hpp` and link against the `PHAST`
CMake target:

```cpp
#include "phast.hpp"

std::vector<uint64_t> keys = /* distinct keys */;
phast::default_function f;
f.build(keys);

uint64_t pos = f(keys[0]);   // pos is in [0, keys.size())
```

To use this project as a CMake dependency, add it as a subdirectory and link
the interface target:

```cmake
add_subdirectory(phast)
target_link_libraries(my_target PRIVATE PHAST)
```

Functions can be saved and reloaded with `essentials::save` / `essentials::load`:

```cpp
essentials::save(f, "mphf.bin");

phast::default_function g;
essentials::load(g, "mphf.bin");
```
