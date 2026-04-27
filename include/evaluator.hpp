#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace phast {

/*
    Bucket-to-activate evaluator.

    PHast's builder maintains a sliding window of buckets and, at each step,
    activates the bucket that maximizes a score. The score depends on the
    bucket's size (number of keys it contains) and its position in the bucket
    array; it is an integer that is increasing in size and decreasing in position.

    `weights` returns score = sw[bucket_size - 1] - 1024 * bucket_nr, where the
    `sw` table is precomputed for the given (bits_per_seed, slice_len) pair.

    The tables come directly from the Rust reference implementation.
*/

struct weights {
    std::array<int32_t, 7> w{};

    weights() = default;

    static weights make(uint8_t bits_per_seed, uint16_t slice_len) {
        weights wt;
        if (slice_len <= 256) {
            if (bits_per_seed <= 4) {
                if (slice_len <= 32)
                    wt.w = {-64542, 121567, 125058, 126982, 128486, 129929, 131003};
                else if (slice_len <= 64)
                    wt.w = {-64511, 116865, 123821, 127467, 130311, 132528, 134191};
                else if (slice_len <= 128)
                    wt.w = {-64100, 107340, 121197, 128499, 133718, 137312, 140441};
                else
                    wt.w = {-73492, 86604, 113513, 128141, 138220, 145456, 151294};
            } else if (bits_per_seed <= 6) {
                if (slice_len <= 32)
                    wt.w = {-63646, 124629, 127000, 128169, 129621, 130183, 130981};
                else if (slice_len <= 64)
                    wt.w = {-63968, 120034, 125091, 127987, 130094, 131516, 132634};
                else if (slice_len <= 128)
                    wt.w = {-64639, 112284, 121682, 127366, 131200, 134360, 136609};
                else
                    wt.w = {-72990, 97195, 115735, 127046, 134403, 140267, 144429};
            } else if (bits_per_seed == 7) {
                if (slice_len <= 32)
                    wt.w = {-60034, 117057, 129045, 130280, 131078, 131608, 132110};
                else if (slice_len <= 64)
                    wt.w = {-63025, 125358, 128028, 129964, 131317, 132267, 132817};
                else if (slice_len <= 128)
                    wt.w = {-64675, 118668, 124723, 128169, 130578, 132997, 133906};
                else
                    wt.w = {-71069, 108154, 121466, 128466, 133053, 137026, 138821};
            } else if (bits_per_seed == 8) {
                if (slice_len <= 32)
                    wt.w = {-60034, 117057, 129045, 130280, 131078, 131608, 132110};
                else if (slice_len <= 64)
                    wt.w = {-61931, 123320, 127144, 129416, 130764, 132175, 132978};
                else if (slice_len <= 128)
                    wt.w = {-64853, 115515, 122738, 127413, 130280, 132894, 134336};
                else
                    wt.w = {-73167, 104363, 117831, 126314, 132226, 137072, 139738};
            } else {
                if (slice_len <= 32)
                    wt.w = {-60034, 117057, 129045, 130280, 131078, 131608, 132110};
                else if (slice_len <= 64)
                    wt.w = {-63025, 125358, 128028, 129964, 131317, 132267, 132817};
                else if (slice_len <= 128)
                    wt.w = {-64675, 118668, 124723, 128169, 130578, 132997, 133906};
                else
                    wt.w = {-71069, 108154, 121466, 128466, 133053, 137026, 138821};
            }
        } else {  // slice_len >= 512
            if (bits_per_seed <= 4) {
                if (slice_len <= 512)
                    wt.w = {-126969, 15686, 67995, 99429, 116711, 218955, 233075};
                else
                    wt.w = {-67844, 12942, 103312, 155604, 191240, 199105, 203210};
            } else if (bits_per_seed == 5) {
                if (slice_len <= 512)
                    wt.w = {-125171, 31908, 74770, 100065, 115115, 126729, 164878};
                else
                    wt.w = {-61359, 22918, 98732, 144970, 180112, 206496, 225555};
            } else if (bits_per_seed == 6) {
                if (slice_len <= 512)
                    wt.w = {-67857, 49430, 91006, 113610, 131179, 139109, 265291};
                else
                    wt.w = {-55666, 36632, 104571, 145873, 173644, 195822, 221577};
            } else if (bits_per_seed == 7) {
                if (slice_len <= 512)
                    wt.w = {-67100, 66220, 100180, 115051, 131394, 142288, 148202};
                else
                    wt.w = {-50734, 49098, 107496, 143459, 169287, 189260, 204132};
            } else if (bits_per_seed == 8) {
                if (slice_len <= 512)
                    wt.w = {-61642, 85224, 112939, 129036, 140809, 150323, 155582};
                else if (slice_len <= 1024)
                    wt.w = {-50171, 59462, 109868, 141865, 163564, 181092, 192852};
                else
                    wt.w = {-1978, 14936, 89762, 150112, 190119, 224213, 343071};
            } else if (bits_per_seed == 9) {
                if (slice_len <= 512)
                    wt.w = {-60668, 86903, 117046, 132208, 140749, 149552, 153428};
                else if (slice_len <= 1024)
                    wt.w = {-58532, 61384, 117335, 146309, 164136, 179495, 187003};
                else
                    wt.w = {-2028, 10459, 102197, 161103, 201199, 227967, 354134};
            } else if (bits_per_seed == 10) {
                if (slice_len <= 512)
                    wt.w = {-65892, 66203, 136361, 155795, 162095, 171627, 174716};
                else if (slice_len <= 1024)
                    wt.w = {-65204, 67367, 119335, 145691, 163238, 179459, 185645};
                else
                    wt.w = {-1683, 8322, 119258, 171679, 203830, 233213, 320945};
            } else if (bits_per_seed == 11) {
                if (slice_len <= 512)
                    wt.w = {-64904, 67974, 141210, 154142, 162631, 171673, 174504};
                else if (slice_len <= 1024)
                    wt.w = {-63000, 69496, 123197, 147274, 164471, 179677, 184910};
                else
                    wt.w = {-1566, 8599, 116024, 185394, 213039, 237292, 249657};
            } else {
                if (slice_len <= 512)
                    wt.w = {-64904, 67974, 141210, 154142, 162631, 171673, 174504};
                else if (slice_len <= 1024)
                    wt.w = {-63000, 69496, 123197, 147274, 164471, 179677, 184910};
                else
                    wt.w = {-1566, 8599, 116024, 185394, 213039, 237292, 249657};
            }
        }
        return wt;
    }

    /* Returns the activation score for a bucket of given size at given position.
       Larger is better. */
    inline int64_t eval(size_t bucket_nr, size_t bucket_size) const {
        int64_t sw;
        if (bucket_size <= w.size()) {
            sw = static_cast<int64_t>(w[bucket_size - 1]);
        } else {
            int64_t l = w[w.size() - 1];
            int64_t p = w[w.size() - 2];
            sw = l + (l - p) * static_cast<int64_t>(bucket_size - w.size());
        }
        return sw - static_cast<int64_t>(1024) * static_cast<int64_t>(bucket_nr);
    }
};

}  // namespace phast
