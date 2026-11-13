// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Dsp/FloatingPointHelper.cs (Ryubing fork, MIT). Static class -> struct with static members.
// MathF.Round (C#) rounds half to even by default, which is std::nearbyint under the default rounding mode.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <emmintrin.h>

#include "../audren_common.h"
#include "nxrt/config.h"

namespace nxrt::audren {

// [audio] dsp_fast_round (default true): the DSP rounds each sample with one SSE2 conversion instead of the C runtime's nearbyint. The
// runtime's version reads the floating-point environment on every call (fegetround): with it the mixer's per-sample rounding was about
// four fifths of the audio thread's time, 0.2 of a core at any frame rate. `false`: std::nearbyint, as in 0.1.8.
inline bool DspFastRound() {
    static const bool on = nxrt::config_get_bool("audio", "dsp_fast_round", true);
    return on;
}

// std::nearbyint for a float, bit for bit: the conversion rounds in the current rounding mode (half to even by default), as nearbyint does.
// A value of 2^23 or more (and infinity, NaN) is whole already and comes back unchanged; the sign is kept, so -0.3 gives -0.0.
inline float RoundToEven(float a) {
    if (!DspFastRound()) return std::nearbyint(a);
    const __m128 x = _mm_set_ss(a);
    const __m128 sign = _mm_and_ps(x, _mm_set_ss(-0.0f));
    if (!(_mm_cvtss_f32(_mm_andnot_ps(_mm_set_ss(-0.0f), x)) < 8388608.0f)) return a;
    return _mm_cvtss_f32(_mm_or_ps(_mm_cvtepi32_ps(_mm_cvtps_epi32(x)), sign));
}

struct FloatingPointHelper {
    static float MultiplyRoundDown(float a, float b) { return RoundDown(a * b); }

    static float RoundDown(float a) { return RoundToEven(a); }

    static float RoundUp(float a) { return RoundToEven(a); }

    static float MultiplyRoundUp(float a, float b) { return RoundUp(a * b); }

    static float Pow10(float x) {
        // NOTE: Nintendo implementation uses Q15 and a LUT for this, we don't.
        // As such, we support the same ranges as Nintendo to avoid unexpected behaviours.
        if (x >= 0.0f) return 1.0f;
        if (x <= -5.3f) return 0.0f;
        return std::pow(10.0f, x);
    }

    static float Log10(float x) {
        // NOTE: Nintendo uses an approximation of log10, we don't.
        // As such, we support the same ranges as Nintendo to avoid unexpected behaviours.
        return std::log10(std::max(x, 1.0e-10f));
    }

    static float MeanSquare(Span<const float> inputs) {
        float res = 0.0f;
        for (float input : inputs) {
            float normInput = input * (1.0f / 32768.0f);
            res += normInput * normInput;
        }
        res /= static_cast<float>(inputs.Length);
        return res;
    }

    // Map decibel to linear.
    static float DecibelToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

    static float DegreesToRadians(float degrees) { return degrees * 3.14159265358979323846f / 180.0f; }

    static float Cos(float value) { return std::cos(DegreesToRadians(value)); }

    static float Sin(float value) { return std::sin(DegreesToRadians(value)); }
};

}  // namespace nxrt::audren
