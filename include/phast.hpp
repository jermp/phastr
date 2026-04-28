#pragma once

/*
    PHast: Perfect Hashing made fast.

    A C++17 translation of the PHast minimal-perfect-hash function originally
    written in Rust by Piotr Beling and Peter Sanders, see:
        Piotr Beling, Peter Sanders. "PHast - Perfect Hashing made fast", 2025.
        https://arxiv.org/abs/2504.17918
        https://github.com/beling/bsuccinct-rs/tree/main/internal/phast

    This translation depends on:
        - jermp/bits       (compact_vector, elias_fano)
        - jermp/essentials (saver/loader/sizer visitors)

    Public API: see `phast::function` in include/function.hpp.
*/

#include "conf.hpp"
#include "cyclic.hpp"
#include "evaluator.hpp"
#include "seeds.hpp"
#include "seed_chooser.hpp"
#include "builder.hpp"
#include "compressed_array.hpp"
#include "hasher.hpp"
#include "function.hpp"
#include "bench_util.hpp"

namespace phast {

/* Convenience alias for the most common configuration:
   - default 64-bit hasher
   - 8-bit seeds
   - regular PHast (SeedOnly chooser)
   - Elias-Fano compressed array */
using default_function = function<>;

}  // namespace phast
