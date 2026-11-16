#pragma once

/*
    PHastR: Faster Minimal Perfect Hashing with Rotated Layouts.

    A C++17 translation of the PHastR minimal-perfect-hash function
    originally written in Rust by Sebastiano Vigna, see:

        Sebastiano Vigna. "PHastR: Faster Minimal Perfect Hashing with
        Rotated Layouts", 2026. https://arxiv.org/abs/2610.17833
        https://github.com/vigna/sux-rs

    Public API: see phastr::function in include/phastr/function.hpp.
*/

#include "util.hpp"
#include "geometry.hpp"
#include "store.hpp"
#include "sweep.hpp"
#include "partitioned.hpp"
#include "builder.hpp"
#include "seed_store.hpp"
#include "function.hpp"

namespace phastr {

/* The most common configuration: 64-bit integer keys, 8-bit seeds stored
   as bytes. */
using default_function = function<>;

}  // namespace phastr
