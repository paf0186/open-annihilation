// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/game_math.hpp"
#include "oa/base/game_math/experimental.hpp"
#include "integer_product.hpp"
#include "rotation.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER) && defined(__x86_64__)
#include <immintrin.h>
#define OA_NATIVE_EXACT_ROTATION 1
#endif

namespace oa::base::game_math {
namespace {
bool experimental_math_enabled_value = false;

// The exact binary64 constants 3.1c uses; do not replace them with ones
// derived from a different pi.
constexpr double radians_to_heading = 10430.37835047;
constexpr double radians_per_angle_word = 9.587379924285e-05;
// How close to a half-integer a heading estimate may come before the exact
// heading decides; far wider than the estimate's error.
constexpr double heading_estimate_margin = 0x1p-28;

// Constants of the game's 32-bit wrapped generator. The seed XOR value is a
// scrambling mask with no known external meaning.
constexpr uint32_t random_multiplier = 16807;
constexpr uint32_t random_quotient_divisor = 127773;
constexpr uint32_t random_modulus = 0x7fffffff;
constexpr uint32_t seed_scrambling_mask = 0x66e29572;
constexpr uint32_t force_odd_seed_bit = 1;

// Separate stores preserve binary64 operations and prevent fused multiply-add.
double rounded(double value) noexcept {
    volatile double result = value;
    return result;
}

// The sine and cosine of the angle of every angle word from 0 to 32768. The
// angle of a negative word is the negative of its magnitude's angle, so its
// sine is negated and its cosine the same.
constexpr std::size_t angle_word_entries = 32769;

struct AngleWordTable {
    std::array<uint64_t, angle_word_entries> sine_significand{};
    std::array<uint64_t, angle_word_entries> cosine_significand{};
    std::array<int16_t, angle_word_entries> sine_exponent{};
    std::array<int16_t, angle_word_entries> cosine_exponent{};
    std::array<uint8_t, angle_word_entries> cosine_negative{};

    AngleWordTable() noexcept {
        for (std::size_t word = 0; word < angle_word_entries; ++word) {
            const double radians = rounded(static_cast<double>(word) * radians_per_angle_word);
            const SineCosine values = sine_cosine(radians);
            sine_significand[word] = values.sine.significand;
            sine_exponent[word] = static_cast<int16_t>(values.sine.exponent);
            cosine_significand[word] = values.cosine.significand;
            cosine_exponent[word] = static_cast<int16_t>(values.cosine.exponent);
            cosine_negative[word] = values.cosine.negative ? 1 : 0;
        }
    }
};

/// Returns the angle table, building its immutable values on first use.
///
/// @return the shared table of exact sine and cosine values
const AngleWordTable& angle_word_table() noexcept {
    static const AngleWordTable table;
    return table;
}

SineCosine angle_word_sine_cosine(int16_t angle) noexcept {
    const auto& table = angle_word_table();
    const bool negative = angle < 0;
    const auto word = static_cast<std::size_t>(negative ? -int32_t{angle} : int32_t{angle});
    return {
        {table.sine_significand[word], table.sine_exponent[word], negative},
        {table.cosine_significand[word],
         table.cosine_exponent[word],
         table.cosine_negative[word] != 0}
    };
}

struct Binary64AngleWordTable {
    std::array<double, angle_word_entries> sine{};
    std::array<double, angle_word_entries> cosine{};

    /// Rounds the exact angle table to binary64 coefficients.
    Binary64AngleWordTable() noexcept {
        const auto& table = angle_word_table();
        for (std::size_t word = 0; word < angle_word_entries; ++word) {
            sine[word] =
                to_double({table.sine_significand[word], table.sine_exponent[word], false});
            cosine[word] = to_double(
                {table.cosine_significand[word],
                 table.cosine_exponent[word],
                 table.cosine_negative[word] != 0}
            );
        }
    }
};

/// Rotates coordinates with separately rounded binary64 products and sums.
///
/// @param first first coordinate
/// @param second second coordinate
/// @param angle angle word, 65536 per turn
/// @return first * cos - second * sin and first * sin + second * cos
RotatedPair binary64_rotate_pair(int32_t first, int32_t second, int16_t angle) noexcept {
    static const Binary64AngleWordTable table;
    const bool negative = angle < 0;
    const auto word = static_cast<std::size_t>(negative ? -int32_t{angle} : int32_t{angle});
    const double sine = negative ? -table.sine[word] : table.sine[word];
    const double cosine = table.cosine[word];
    const double cosine_first = rounded(cosine * static_cast<double>(first));
    const double sine_second = rounded(sine * static_cast<double>(second));
    const double sine_first = rounded(sine * static_cast<double>(first));
    const double cosine_second = rounded(cosine * static_cast<double>(second));
    return {rounded(cosine_first - sine_second), rounded(sine_first + cosine_second)};
}

#if defined(OA_NATIVE_EXACT_ROTATION)
struct SplitCoefficient {
    double high{};
    double low{};
};

struct SplitAngle {
    SplitCoefficient sine{};
    SplitCoefficient cosine{};
};

struct SplitAngleWordTable {
    std::array<SplitAngle, angle_word_entries> values{};
    bool supported = true;

    /// Splits every exact coefficient into two exactly representable doubles.
    SplitAngleWordTable() noexcept {
        const auto& table = angle_word_table();
        for (std::size_t word = 0; word < angle_word_entries; ++word) {
            values[word].sine =
                split(table.sine_significand[word], table.sine_exponent[word], false);
            values[word].cosine = split(
                table.cosine_significand[word],
                table.cosine_exponent[word],
                table.cosine_negative[word] != 0
            );
        }
    }

    /// Splits a coefficient, requiring both parts and their products to stay normal.
    ///
    /// @param significand normalized significand, or zero
    /// @param exponent exponent of the leading bit
    /// @param negative sign of the coefficient
    /// @return exact high 53 bits and low 11 bits, including signed zeros
    SplitCoefficient split(uint64_t significand, int32_t exponent, bool negative) noexcept {
        constexpr uint64_t leading_bit = uint64_t{1} << 63;
        constexpr uint64_t fraction_mask = 0xfffffffffffffULL;
        const uint64_t sign = negative ? leading_bit : 0;
        if (significand == 0)
            return {std::bit_cast<double>(sign), std::bit_cast<double>(sign)};
        if ((significand & leading_bit) == 0 || exponent < -959 || exponent > 0) {
            supported = false;
            return {};
        }
        const double high = std::bit_cast<double>(
            sign | (static_cast<uint64_t>(exponent + 1023) << 52) |
            ((significand >> 11) & fraction_mask)
        );
        const uint64_t low = significand & 0x7ff;
        if (low == 0)
            return {high, std::bit_cast<double>(sign)};
        const unsigned top = 63 - static_cast<unsigned>(std::countl_zero(low));
        const int32_t low_exponent = exponent - 63 + static_cast<int32_t>(top);
        return {
            high,
            std::bit_cast<double>(
                sign | (static_cast<uint64_t>(low_exponent + 1023) << 52) |
                ((low << (52 - top)) & fraction_mask)
            )
        };
    }
};

/// Returns an exact split table only on processors that support native rounding control.
///
/// @return the supported table, or null for the integer product path
const SplitAngleWordTable* native_rotation_table() noexcept {
    static const bool processor_supported =
        __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("fma");
    if (!processor_supported)
        return nullptr;
    static const SplitAngleWordTable table;
    return table.supported ? &table : nullptr;
}

/// Rotates with native products rounded to nearest without product exceptions.
///
/// Each coefficient is exactly high + low, with 53 and at most 11 bits.
/// A low part times a 32-bit coordinate needs at most 43 significant bits,
/// so that product is exact. Table bounds keep nonzero parts and products
/// normal. Fusing high * coordinate + low_product rounds the original
/// coefficient product once to 53 bits. Explicit nearest-even rounding
/// without exceptions preserves the integer product's result and flags
/// independently of the ambient mode. Final sums retain their usual rounding.
///
/// @param first first coordinate
/// @param second second coordinate
/// @param angle angle word, 65536 per turn
/// @param table exact split coefficients with normal nonzero parts
/// @return the same pair and floating-point flags as the integer product path
__attribute__((target("avx512f,fma"))) RotatedPair native_rotate_pair(
    int32_t first, int32_t second, int16_t angle, const SplitAngleWordTable& table
) noexcept {
    const bool negative = angle < 0;
    const auto word = static_cast<std::size_t>(negative ? -int32_t{angle} : int32_t{angle});
    const auto& entry = table.values[word];
    const double sine_high = negative ? -entry.sine.high : entry.sine.high;
    const double sine_low = negative ? -entry.sine.low : entry.sine.low;
    const double x = static_cast<double>(first), y = static_cast<double>(second);
    const __m128d first_value = _mm_set_sd(x), second_value = _mm_set_sd(y);
    constexpr int rounding = _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC;
    const double cosine_first = _mm_cvtsd_f64(_mm_fmadd_round_sd(
        _mm_set_sd(entry.cosine.high), first_value, _mm_set_sd(entry.cosine.low * x), rounding
    ));
    const double sine_second = _mm_cvtsd_f64(
        _mm_fmadd_round_sd(_mm_set_sd(sine_high), second_value, _mm_set_sd(sine_low * y), rounding)
    );
    const double sine_first = _mm_cvtsd_f64(
        _mm_fmadd_round_sd(_mm_set_sd(sine_high), first_value, _mm_set_sd(sine_low * x), rounding)
    );
    const double cosine_second = _mm_cvtsd_f64(_mm_fmadd_round_sd(
        _mm_set_sd(entry.cosine.high), second_value, _mm_set_sd(entry.cosine.low * y), rounding
    ));
    return {rounded(cosine_first - sine_second), rounded(sine_first + cosine_second)};
}
#endif
} // namespace

void set_experimental_math(bool enabled) noexcept {
    experimental_math_enabled_value = enabled;
}

bool experimental_math_enabled() noexcept {
    return experimental_math_enabled_value;
}

uint32_t random_bounded(uint32_t& state, uint32_t bound) noexcept {
    if (std::bit_cast<int32_t>(bound) < 2)
        return 0;
    state = state * random_multiplier - (state / random_quotient_divisor) * random_modulus;
    if (std::bit_cast<int32_t>(state) < 1)
        state += random_modulus;
    return state % bound;
}

void seed_random(uint32_t& state, uint32_t input) noexcept {
    state = (input ^ seed_scrambling_mask) | force_odd_seed_bit;
}

uint16_t direction(int32_t x, int32_t z) noexcept {
    // An estimate within 2^-35 of the heading settles the nearest integer
    // unless it lies within heading_estimate_margin of a half-integer.
    const double estimate =
        approximate_arctangent(static_cast<double>(x), static_cast<double>(z)) * radians_to_heading;
    const double estimate_lower = std::floor(estimate);
    const double estimate_fraction = estimate - estimate_lower;
    if (std::fabs(estimate_fraction - 0.5) > heading_estimate_margin)
        return static_cast<uint16_t>(
            static_cast<int32_t>(estimate_lower + (estimate_fraction > 0.5 ? 1.0 : 0.0))
        );
    const double angle = to_double(multiply(
        arctangent(static_cast<double>(x), static_cast<double>(z)),
        to_extended(radians_to_heading),
        Precision::bits_53
    ));
    const double lower = std::floor(angle), fraction = angle - lower;
    const auto nearest =
        lower + ((fraction > 0.5 || (fraction == 0.5 && std::fmod(lower, 2.0) != 0)) ? 1.0 : 0.0);
    return static_cast<uint16_t>(static_cast<int32_t>(nearest));
}

std::optional<Extended> arccosine(double x) noexcept {
    if (!(x >= -1.0 && x <= 1.0))
        return std::nullopt;
    if (x == 1.0)
        return to_extended(0.0);
    if (x == -1.0)
        return arctangent(0.0, -1.0);
    // Sum, difference, product and root are each rounded to a double.
    const double sum = rounded(1.0 + x), difference = rounded(1.0 - x);
    return arctangent(square_root(rounded(sum * difference)), x);
}

double square_root(double value) noexcept {
    // IEEE 754 rounds the square root exactly, like the four basic operations.
    return std::sqrt(value);
}

RotatedPair detail::integer_rotate_pair(int32_t first, int32_t second, int16_t angle) noexcept {
    const SineCosine angle_values = angle_word_sine_cosine(angle);
    const Extended sine_value = angle_values.sine, cosine_value = angle_values.cosine;
    return {
        rounded(
            detail::integer_product(cosine_value, first) -
            detail::integer_product(sine_value, second)
        ),
        rounded(
            detail::integer_product(sine_value, first) +
            detail::integer_product(cosine_value, second)
        )
    };
}

bool detail::native_rotation_available() noexcept {
#if defined(OA_NATIVE_EXACT_ROTATION)
    return native_rotation_table() != nullptr;
#else
    return false;
#endif
}

RotatedPair rotate_pair(int32_t first, int32_t second, int16_t angle) noexcept {
    if (experimental_math_enabled_value)
        return binary64_rotate_pair(first, second, angle);
#if defined(OA_NATIVE_EXACT_ROTATION)
    if (const auto* table = native_rotation_table())
        return native_rotate_pair(first, second, angle, *table);
#endif
    return detail::integer_rotate_pair(first, second, angle);
}

double planar_length(int32_t x, int32_t z) noexcept {
    return hypotenuse(static_cast<double>(x), static_cast<double>(z));
}

uint32_t distance(int32_t x, int32_t z) noexcept {
    return static_cast<uint32_t>(static_cast<uint64_t>(planar_length(x, z)));
}

void scale_vector_fixed(int32_t out[3], const int32_t in[3], int32_t scale) noexcept {
    for (int i = 0; i < 3; ++i) {
        const auto product = static_cast<int64_t>(in[i]) * static_cast<int64_t>(scale);
        out[i] = static_cast<int32_t>(product >> 16);
    }
}

int32_t truncated_length(int32_t x, int32_t y, int32_t z) noexcept {
    const auto square = [](int32_t v) {
        return rounded(static_cast<double>(v) * static_cast<double>(v));
    };
    const double sum = rounded(rounded(square(z) + square(y)) + square(x));
    const auto wide = static_cast<int64_t>(square_root(sum));
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(wide)));
}

int32_t squared_magnitude_high(int32_t x, int32_t y, int32_t z) noexcept {
    const auto high = [](int32_t value) {
        return static_cast<int32_t>(
            static_cast<uint64_t>(static_cast<int64_t>(value) * static_cast<int64_t>(value)) >> 32
        );
    };
    return high(x) + high(y) + high(z);
}

void float_heap_sift_up(
    FloatHeapPair* heap, int32_t hole, int32_t lower, uint32_t value, float key
) noexcept {
    while (lower < hole && heap[(hole - 1) / 2].key < key) {
        const auto parent = (hole - 1) / 2;
        heap[hole] = heap[parent];
        hole = parent;
    }
    heap[hole].value = value;
    heap[hole].key = key;
}

void float_heap_replace(
    FloatHeapPair* heap, int32_t start, int32_t size, uint32_t value, float key
) noexcept {
    auto hole = start;
    auto child = 0;
    while (true) {
        const auto doubled = hole * 2;
        child = doubled + 2;
        if (size <= child)
            break;
        if (heap[child].key < heap[child - 1].key)
            child = doubled + 1;
        heap[hole] = heap[child];
        hole = child;
    }
    if (child == size) {
        heap[hole] = heap[child - 1];
        hole = hole * 2 + 1;
    }
    float_heap_sift_up(heap, hole, start, value, key);
}

void float_heap_extract(
    FloatHeapPair* heap, int32_t size, FloatHeapPair* out, uint32_t value, float key
) noexcept {
    *out = *heap;
    float_heap_replace(heap, 0, size, value, key);
}

void float_heap_make(FloatHeapPair* heap, int32_t size) noexcept {
    for (auto hole = size / 2; hole > 0;) {
        --hole;
        const auto pair = heap[hole];
        float_heap_replace(heap, hole, size, pair.value, pair.key);
    }
}

void float_heap_pop(FloatHeapPair* heap, int32_t size) noexcept {
    const auto last = heap[size - 1];
    float_heap_extract(heap, size - 1, &heap[size - 1], last.value, last.key);
}
} // namespace oa::base::game_math
