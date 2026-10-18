// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/game_math.hpp"
#include "oa/base/game_math/experimental.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void experimental_rotations() {
    namespace gm = oa::base::game_math;
    constexpr std::array<std::array<int32_t, 2>, 12> coordinates{
        {{0, 0},
         {0, 1},
         {1, 0},
         {1, 1},
         {-1, 1},
         {13, 7},
         {32767, 32768},
         {65535, 65536},
         {INT32_MAX, INT32_MAX},
         {INT32_MIN, INT32_MAX},
         {INT32_MIN, INT32_MIN},
         {INT32_MAX, 0}}
    };
    constexpr std::size_t angles = 65536;
    using Results = std::array<gm::RotatedPair, coordinates.size()>;
    std::vector<Results> baseline(angles);
    const auto same = [](gm::RotatedPair a, gm::RotatedPair b) {
        return std::bit_cast<uint64_t>(a.first) == std::bit_cast<uint64_t>(b.first) &&
               std::bit_cast<uint64_t>(a.second) == std::bit_cast<uint64_t>(b.second);
    };
    if (gm::experimental_math_enabled())
        throw std::runtime_error("experimental rotations must default to disabled");
    for (std::size_t word = 0; word < angles; ++word) {
        const int16_t angle = std::bit_cast<int16_t>(static_cast<uint16_t>(word));
        for (std::size_t i = 0; i < coordinates.size(); ++i)
            baseline[word][i] = gm::rotate_pair(coordinates[i][0], coordinates[i][1], angle);
    }
    gm::set_experimental_math(true);
    if (!gm::experimental_math_enabled())
        throw std::runtime_error("experimental rotations were not enabled");
    double maximum_error = 0.0;
    uint64_t changed = 0, changed_truncations = 0, changed_zero_signs = 0;
    for (std::size_t word = 0; word < angles; ++word) {
        const int16_t angle = std::bit_cast<int16_t>(static_cast<uint16_t>(word));
        for (std::size_t i = 0; i < coordinates.size(); ++i) {
            const int32_t first = coordinates[i][0], second = coordinates[i][1];
            const auto actual = gm::rotate_pair(first, second, angle);
            if (!same(actual, gm::rotate_pair(first, second, angle)))
                throw std::runtime_error("experimental rotations do not repeat exactly");
            const std::array<double, 2> output{actual.first, actual.second};
            const std::array<double, 2> expected{baseline[word][i].first, baseline[word][i].second};
            for (std::size_t j = 0; j < output.size(); ++j) {
                const double error = std::fabs(output[j] - expected[j]);
                // A binary64 coefficient changes by at most 2^-53. With
                // signed 32-bit coordinates, separately rounded products
                // and sums keep the absolute error below this bound.
                if (!std::isfinite(output[j]) || error > 0x1p-18)
                    throw std::runtime_error("experimental rotation exceeds its error bound");
                if (error > maximum_error)
                    maximum_error = error;
                changed +=
                    std::bit_cast<uint64_t>(output[j]) != std::bit_cast<uint64_t>(expected[j]);
                changed_truncations += std::trunc(output[j]) != std::trunc(expected[j]);
                changed_zero_signs += output[j] == 0.0 && expected[j] == 0.0 &&
                                      std::signbit(output[j]) != std::signbit(expected[j]);
            }
        }
    }
    gm::set_experimental_math(false);
    if (gm::experimental_math_enabled())
        throw std::runtime_error("experimental rotations were not disabled");
    for (std::size_t word = 0; word < angles; ++word) {
        const int16_t angle = std::bit_cast<int16_t>(static_cast<uint16_t>(word));
        for (std::size_t i = 0; i < coordinates.size(); ++i)
            if (!same(
                    baseline[word][i], gm::rotate_pair(coordinates[i][0], coordinates[i][1], angle)
                ))
                throw std::runtime_error("disabling experimental rotations changed normal results");
    }
    std::printf(
        "experimental rotations: %llu changed components, %llu changed truncations, "
        "%llu changed zero signs, maximum absolute error %.17g\n",
        static_cast<unsigned long long>(changed),
        static_cast<unsigned long long>(changed_truncations),
        static_cast<unsigned long long>(changed_zero_signs),
        maximum_error
    );
}

/// Checks truncate_to_int64 and truncate_low32 at zero, the halves, the
/// 32-bit, 32-bit-plus-one and 64-bit edges, and NaN and the infinities.
void truncation() {
    using oa::base::game_math::truncate_low32;
    using oa::base::game_math::truncate_to_int64;
    constexpr double two_31 = 2147483648.0;
    constexpr double two_32 = 4294967296.0;
    constexpr double two_63 = 9223372036854775808.0;
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double infinity = std::numeric_limits<double>::infinity();
    static_assert(truncate_to_int64(0.5) == 0 && truncate_to_int64(-0.5) == 0);
    static_assert(truncate_to_int64(1.75) == 1 && truncate_to_int64(-1.75) == -1);
    static_assert(truncate_low32(0.5) == 0 && truncate_low32(-0.5) == 0);
    // 2^31 wraps to INT32_MIN; -2^31 is INT32_MIN itself.
    static_assert(truncate_to_int64(two_31) == 2147483648LL);
    static_assert(truncate_low32(two_31) == INT32_MIN && truncate_low32(-two_31) == INT32_MIN);
    // 2^32 + 1 keeps 1; its negation keeps -1.
    static_assert(truncate_low32(two_32 + 1.0) == 1 && truncate_low32(-(two_32 + 1.0)) == -1);
    // -2^63 is in range; 2^63 is not.
    static_assert(truncate_to_int64(-two_63) == INT64_MIN && truncate_low32(-two_63) == 0);
    static_assert(truncate_to_int64(two_63) == INT64_MIN && truncate_low32(two_63) == 0);
    static_assert(truncate_to_int64(9223372036854774784.0) == 9223372036854774784LL);
    static_assert(truncate_low32(9223372036854774784.0) == static_cast<int32_t>(0xFFFFFC00u));
    static_assert(truncate_to_int64(nan) == INT64_MIN && truncate_low32(nan) == 0);
    static_assert(truncate_to_int64(infinity) == INT64_MIN && truncate_low32(infinity) == 0);
    static_assert(truncate_to_int64(-infinity) == INT64_MIN && truncate_low32(-infinity) == 0);
    // The same at run time, where the compiler cannot fold the conversions.
    volatile double runtime_nan = nan;
    volatile double runtime_wrap = two_32 + 7.9;
    if (truncate_low32(runtime_nan) != 0 || truncate_to_int64(runtime_nan) != INT64_MIN ||
        truncate_low32(runtime_wrap) != 7)
        throw std::runtime_error("run-time truncation differs");
    // A float converts to double exactly.
    if (truncate_low32(3.75F) != 3 || truncate_low32(-3.75F) != -3)
        throw std::runtime_error("float truncation differs");
}

} // namespace

int main() {
    truncation();
    experimental_rotations();
    uint32_t rng_state = 1;
    if (oa::base::game_math::random_bounded(rng_state, 100) != 7 || rng_state != 16807)
        throw std::runtime_error("random first state and draw");
    if (oa::base::game_math::random_bounded(rng_state, 100) != 49 || rng_state != 282475249)
        throw std::runtime_error("random state continuity");
    const auto saved_rng = rng_state;
    for (auto bound : {0u, 1u, 0x80000000u, 0xffffffffu})
        if (oa::base::game_math::random_bounded(rng_state, bound) != 0 || rng_state != saved_rng)
            throw std::runtime_error("bounds below 2, read as signed, do not advance the state");
    rng_state = 0;
    if (oa::base::game_math::random_bounded(rng_state, 2) != 1 || rng_state != 0x7fffffffu)
        throw std::runtime_error("a zero state is raised by 2^31 - 1");
    oa::base::game_math::seed_random(rng_state, 0);
    if (rng_state != 0x66e29573u)
        throw std::runtime_error("seed transform");
    if (oa::base::game_math::direction(1, 0) != 16384 ||
        oa::base::game_math::direction(-1, 0) != 49152 ||
        oa::base::game_math::distance(3, 4) != 5 || oa::base::game_math::distance(0, 0) != 0)
        throw std::runtime_error("direction/distance endpoints");
    int32_t in[3] = {0x20000, -0x10000, 0x30000};
    int32_t out[3]{};
    oa::base::game_math::scale_vector_fixed(out, in, 0x8000);
    if (out[0] != 0x10000 || out[1] != -0x8000 || out[2] != 0x18000)
        throw std::runtime_error("half-scale");
    if (oa::base::game_math::truncated_length(3, 4, 12) != 13)
        throw std::runtime_error("3-4-12 length");
    // The length truncates to 64 bits and the low word is kept, so a length
    // past 2^31 (3719550785 here) wraps negative instead of saturating.
    if (oa::base::game_math::truncated_length(0x7fffffff, 0x7fffffff, 0x7fffffff) != -575416511)
        throw std::runtime_error("low word of the 64-bit length");
    // The squares sum exactly to 456698230^2. Adding z^2 + y^2 first lands on
    // the double nearest that sum; x^2 + y^2 first lands one double lower,
    // whose root truncates to 456698229.
    if (oa::base::game_math::truncated_length(456698220, 93580, 19410) != 456698230)
        throw std::runtime_error("z^2 + y^2 before x^2");
    if (oa::base::game_math::squared_magnitude_high(65536, 65536, 0) != 2)
        throw std::runtime_error("high squares of a unit square");
    if (oa::base::game_math::squared_magnitude_high(-65536, 0, 0) != 1)
        throw std::runtime_error("high square of a negative");
    oa::base::game_math::FloatHeapPair heap[3]{};
    oa::base::game_math::float_heap_sift_up(heap, 0, 0, 1, 1.0F);
    oa::base::game_math::float_heap_sift_up(heap, 1, 0, 2, 3.0F);
    oa::base::game_math::float_heap_sift_up(heap, 2, 0, 3, 2.0F);
    if (heap[0].value != 2 || heap[0].key != 3.0F || heap[1].value != 1 || heap[2].value != 3)
        throw std::runtime_error("max-heap insert");
    oa::base::game_math::float_heap_replace(heap, 0, 3, 4, 0.5F);
    if (heap[0].value != 3 || heap[0].key != 2.0F || heap[1].value != 1 || heap[2].value != 4 ||
        heap[2].key != 0.5F)
        throw std::runtime_error("replace root");
    oa::base::game_math::FloatHeapPair popped[3]{};
    oa::base::game_math::float_heap_sift_up(popped, 0, 0, 1, 1.0F);
    oa::base::game_math::float_heap_sift_up(popped, 1, 0, 2, 3.0F);
    oa::base::game_math::float_heap_sift_up(popped, 2, 0, 3, 2.0F);
    oa::base::game_math::float_heap_extract(popped, 2, &popped[2], popped[2].value, popped[2].key);
    if (popped[0].value != 3 || popped[0].key != 2.0F || popped[1].value != 1 ||
        popped[1].key != 1.0F || popped[2].value != 2 || popped[2].key != 3.0F)
        throw std::runtime_error("extract root");

    // Making the heap re-places parents 1 then 0; each hole sinks to the bottom
    // before the pair climbs back up.
    oa::base::game_math::FloatHeapPair made[5]{
        {10, 1.0F}, {11, 3.0F}, {12, 2.0F}, {13, 5.0F}, {14, 4.0F}
    };
    oa::base::game_math::float_heap_make(made, 5);
    const uint32_t made_values[5]{13, 14, 12, 11, 10};
    const float made_keys[5]{5.0F, 4.0F, 2.0F, 3.0F, 1.0F};
    for (int i = 0; i < 5; ++i)
        if (made[i].value != made_values[i] || made[i].key != made_keys[i])
            throw std::runtime_error("make heap");
    // Popping moves the root to the last slot; the old last pair re-enters
    // the four-element heap from the root.
    oa::base::game_math::float_heap_pop(made, 5);
    const uint32_t popped_values[5]{14, 11, 12, 10, 13};
    const float popped_keys[5]{4.0F, 3.0F, 2.0F, 1.0F, 5.0F};
    for (int i = 0; i < 5; ++i)
        if (made[i].value != popped_values[i] || made[i].key != popped_keys[i])
            throw std::runtime_error("pop heap");
    // On equal keys replacing takes the right child.
    oa::base::game_math::FloatHeapPair tied[3]{{1, 2.0F}, {2, 2.0F}, {3, 2.0F}};
    oa::base::game_math::float_heap_make(tied, 3);
    if (tied[0].value != 3 || tied[1].value != 2 || tied[2].value != 1)
        throw std::runtime_error("right child on a tie");
}
