# PHast and PHastR

C++17 implementations of two minimal perfect hash functions (MPHFs) of the
PHast family:

- **PHast** (Perfect Hashing made fast), by Piotr Beling and Peter Sanders:

  > Piotr Beling, Peter Sanders. *PHast - Perfect Hashing made fast*, 2025.
  > [arXiv:2504.17918](https://arxiv.org/abs/2504.17918)

  Reference Rust implementation:
  [beling/bsuccinct-rs](https://github.com/beling/bsuccinct-rs/tree/main/internal/phast).

- **PHastR** (minimal perfect hashing with rotated layouts), by Sebastiano
  Vigna:

  > Sebastiano Vigna. *PHastR: Faster Minimal Perfect Hashing with Rotated
  > Layouts*, 2026. [arXiv:2610.17833](https://arxiv.org/abs/2610.17833)

  Reference Rust implementation:
  [vigna/sux-rs](https://github.com/vigna/sux-rs).

  With the default parameters (8-bit seeds, four layouts, slices of length
  1024, and an expected bucket size of 4.25 keys), PHastR uses about 1.94
  bits per key; it is much faster to build than PHast, and its queries are
  as fast or faster, as it bumps very few keys (about 0.5%). The
  implementation supports multi-threaded construction and an
  external-memory mode that keeps the key records in temporary files and
  builds the *identical* structure, scaling to very large key sets.

This repository provides header-only C++17 translations built on top of
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

The build produces these executables in the `build/` directory:

- `example`      — a small end-to-end demo (from `src/example.cpp`)
- `perf_phast`   — a PHast performance benchmark (from `benchmarks/perf_phast.cpp`)
- `perf_phastr`  — a PHastR performance benchmark (from `benchmarks/perf_phastr.cpp`)
- `test_phast`   — the PHast test suite (from `test/test_phast.cpp`)
- `test_phastr`  — the PHastR test suite (from `test/test_phastr.cpp`)

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

`perf_phastr` does the same for PHastR, with optional multi-threaded
construction and external-memory mode:

```bash
./perf_phastr <num_keys> <num_lookups> <num_runs> <seed> <num_threads> <external>
./perf_phastr 100000000 10000000 1 42 8 0    # 8 threads, internal memory
./perf_phastr 100000000 10000000 1 42 8 1    # 8 threads, external memory
```

## Running the tests

From the `build/` directory:

```bash
ctest --output-on-failure
```

or run the test binaries directly:

```bash
./test_phast
./test_phastr
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

## Using PHastR in your own code

PHastR is header-only as well. Include `phastr/phastr.hpp`:

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

The two build methods read the keys just once and build *identical*
structures. With a random-access iterator the records of the keys are
computed by `config.num_threads` writers in parallel (sharing the memory
of one writer); a forward iterator is read sequentially. The structure
does not depend on the number of threads, nor on the kind of iterator (it
depends on `config.sweep_chunks`, which defaults to `config.num_threads`:
fix it to make the structure independent of the number of threads
used).

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

For key types other than `uint64_t`, supply a hasher with the same
interface as `phastr::default_hasher`: a constructor from a 64-bit seed
(which can preprocess the seed once), a hashing
`uint64_t operator()(Key const&) const`, and a `visit` method serializing
any preprocessed state. Serialization works as for PHast, with
`essentials::save` / `essentials::load`.
