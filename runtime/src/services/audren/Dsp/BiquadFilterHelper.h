// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Dsp/BiquadFilterHelper.cs (Ryubing fork, MIT). The C# file has no SIMD variants; every
// function here is the scalar body of the C# one, with the same float operation order.
//
// Recurrences (DF2 = ProcessBiquadFilter with one parameter, DF1 = the others), with numerator a0..a2 and denominator b1, b2:
//   y[n] = a0 x[n] + a1 x[n-1] + a2 x[n-2] + b1 y[n-1] + b2 y[n-2]
// Field names come from audren_types.h (BiquadFilterParameter2 / BiquadFilterState use lower_snake names there).
#pragma once
#include <cmath>
#include <cstdint>
#include "../audren_common.h"
#include "../audren_types.h"
#include "FloatingPointHelper.h"

namespace nxrt::audren {

class BiquadFilterHelper {
  private:
    static constexpr int32_t FixedPointPrecisionForParameter = 14;

    // Ported locally: Ryujinx's FixedPointHelper.ToFixed / ToFloat and FloatingPointHelper.MultiplyRoundUp, which are
    // outside this port's scope. C# `(int)float` is saturating (NaN -> 0) on current .NET x64, so the same is done here.
    static int32_t FixedPointToFixed(float value, int32_t qBits) {
        float scaled = value * static_cast<float>(1 << qBits);
        if (std::isnan(scaled)) return 0;
        if (scaled >= 2147483648.0f) return INT32_MAX;
        if (scaled <= -2147483648.0f) return INT32_MIN;
        return static_cast<int32_t>(scaled);
    }

    static float FixedPointToFloat(int64_t value, int32_t qBits) {
        return static_cast<float>(value) / static_cast<float>(1 << qBits);
    }

    // C# MathF.Round(a * b): round half to even, like std::nearbyint in the default rounding mode.
    static float FloatingPointMultiplyRoundUp(float a, float b) { return RoundToEven(a * b); }

  public:
    static BiquadFilterParameter1 ToBiquadFilterParameter1(const BiquadFilterParameter2& parameter) {
        BiquadFilterParameter1 result{};
        result.enable = parameter.enable;

        result.numerator[0] = static_cast<int16_t>(FixedPointToFixed(parameter.numerator[0], FixedPointPrecisionForParameter));
        result.numerator[1] = static_cast<int16_t>(FixedPointToFixed(parameter.numerator[1], FixedPointPrecisionForParameter));
        result.numerator[2] = static_cast<int16_t>(FixedPointToFixed(parameter.numerator[2], FixedPointPrecisionForParameter));

        result.denominator[0] = static_cast<int16_t>(FixedPointToFixed(parameter.denominator[0], FixedPointPrecisionForParameter));
        result.denominator[1] = static_cast<int16_t>(FixedPointToFixed(parameter.denominator[1], FixedPointPrecisionForParameter));

        return result;
    }

    static BiquadFilterParameter2 ToBiquadFilterParameter2(const BiquadFilterParameter1& parameter) {
        BiquadFilterParameter2 result{};
        result.enable = parameter.enable;

        result.numerator[0] = FixedPointToFloat(parameter.numerator[0], FixedPointPrecisionForParameter);
        result.numerator[1] = FixedPointToFloat(parameter.numerator[1], FixedPointPrecisionForParameter);
        result.numerator[2] = FixedPointToFloat(parameter.numerator[2], FixedPointPrecisionForParameter);

        result.denominator[0] = FixedPointToFloat(parameter.denominator[0], FixedPointPrecisionForParameter);
        result.denominator[1] = FixedPointToFloat(parameter.denominator[1], FixedPointPrecisionForParameter);

        return result;
    }

    static BiquadFilterEffectParameter1 ToBiquadFilterEffectParameter1(const BiquadFilterEffectParameter2& parameter) {
        BiquadFilterEffectParameter1 result{};
        std::memcpy(result.input, parameter.input, sizeof(result.input));
        std::memcpy(result.output, parameter.output, sizeof(result.output));
        result.channel_count = parameter.channel_count;
        result.status = parameter.status;

        result.numerator[0] = static_cast<int16_t>(FixedPointToFixed(parameter.numerator[0], FixedPointPrecisionForParameter));
        result.numerator[1] = static_cast<int16_t>(FixedPointToFixed(parameter.numerator[1], FixedPointPrecisionForParameter));
        result.numerator[2] = static_cast<int16_t>(FixedPointToFixed(parameter.numerator[2], FixedPointPrecisionForParameter));

        result.denominator[0] = static_cast<int16_t>(FixedPointToFixed(parameter.denominator[0], FixedPointPrecisionForParameter));
        result.denominator[1] = static_cast<int16_t>(FixedPointToFixed(parameter.denominator[1], FixedPointPrecisionForParameter));

        return result;
    }

    static BiquadFilterEffectParameter2 ToBiquadFilterEffectParameter2(const BiquadFilterEffectParameter1& parameter) {
        BiquadFilterEffectParameter2 result{};
        std::memcpy(result.input, parameter.input, sizeof(result.input));
        std::memcpy(result.output, parameter.output, sizeof(result.output));
        result.channel_count = parameter.channel_count;
        result.status = parameter.status;

        result.numerator[0] = FixedPointToFloat(parameter.numerator[0], FixedPointPrecisionForParameter);
        result.numerator[1] = FixedPointToFloat(parameter.numerator[1], FixedPointPrecisionForParameter);
        result.numerator[2] = FixedPointToFloat(parameter.numerator[2], FixedPointPrecisionForParameter);

        result.denominator[0] = FixedPointToFloat(parameter.denominator[0], FixedPointPrecisionForParameter);
        result.denominator[1] = FixedPointToFloat(parameter.denominator[1], FixedPointPrecisionForParameter);

        return result;
    }

    // Direct form 2. Apply a single biquad filter.
    static void ProcessBiquadFilter(BiquadFilterParameter2& parameter, BiquadFilterState& state, Span<float> outputBuffer,
                                    Span<const float> inputBuffer, uint32_t sampleCount) {
        float a0 = parameter.numerator[0];
        float a1 = parameter.numerator[1];
        float a2 = parameter.numerator[2];

        float b1 = parameter.denominator[0];
        float b2 = parameter.denominator[1];

        for (uint32_t i = 0; i < sampleCount; i++) {
            float input = inputBuffer[i];
            float output = input * a0 + state.state0;

            state.state0 = input * a1 + output * b1 + state.state1;
            state.state1 = input * a2 + output * b2;

            outputBuffer[i] = output;
        }
    }

    // Direct form 1. Apply a single biquad filter and mix the result into the output buffer.
    static void ProcessBiquadFilterAndMix(BiquadFilterParameter2& parameter, BiquadFilterState& state, Span<float> outputBuffer,
                                          Span<const float> inputBuffer, uint32_t sampleCount, float volume) {
        float a0 = parameter.numerator[0];
        float a1 = parameter.numerator[1];
        float a2 = parameter.numerator[2];

        float b1 = parameter.denominator[0];
        float b2 = parameter.denominator[1];

        for (uint32_t i = 0; i < sampleCount; i++) {
            float input = inputBuffer[i];
            float output = input * a0 + state.state0 * a1 + state.state1 * a2 + state.state2 * b1 + state.state3 * b2;

            state.state1 = state.state0;
            state.state0 = input;
            state.state3 = state.state2;
            state.state2 = output;

            outputBuffer[i] += FloatingPointMultiplyRoundUp(output, volume);
        }
    }

    // Direct form 1. Apply a single biquad filter and mix the result into the output buffer with volume ramp.
    // Returns the last filtered sample value.
    static float ProcessBiquadFilterAndMixRamp(BiquadFilterParameter2& parameter, BiquadFilterState& state, Span<float> outputBuffer,
                                               Span<const float> inputBuffer, uint32_t sampleCount, float volume, float ramp) {
        float a0 = parameter.numerator[0];
        float a1 = parameter.numerator[1];
        float a2 = parameter.numerator[2];

        float b1 = parameter.denominator[0];
        float b2 = parameter.denominator[1];

        float mixState = 0.0f;

        for (uint32_t i = 0; i < sampleCount; i++) {
            float input = inputBuffer[i];
            float output = input * a0 + state.state0 * a1 + state.state1 * a2 + state.state2 * b1 + state.state3 * b2;

            state.state1 = state.state0;
            state.state0 = input;
            state.state3 = state.state2;
            state.state2 = output;

            mixState = FloatingPointMultiplyRoundUp(output, volume);

            outputBuffer[i] += mixState;
            volume += ramp;
        }

        return mixState;
    }

    // Direct form 1. Apply multiple biquad filters in series; stage 0 reads inputBuffer, later stages read outputBuffer.
    static void ProcessBiquadFilter(Span<const BiquadFilterParameter2> parameters, Span<BiquadFilterState> states,
                                    Span<float> outputBuffer, Span<const float> inputBuffer, uint32_t sampleCount) {
        for (size_t stageIndex = 0; stageIndex < parameters.Length; stageIndex++) {
            const BiquadFilterParameter2& parameter = parameters[stageIndex];

            BiquadFilterState& state = states[stageIndex];

            float a0 = parameter.numerator[0];
            float a1 = parameter.numerator[1];
            float a2 = parameter.numerator[2];

            float b1 = parameter.denominator[0];
            float b2 = parameter.denominator[1];

            for (uint32_t i = 0; i < sampleCount; i++) {
                float input = stageIndex != 0 ? outputBuffer[i] : inputBuffer[i];
                float output = input * a0 + state.state0 * a1 + state.state1 * a2 + state.state2 * b1 + state.state3 * b2;

                state.state1 = state.state0;
                state.state0 = input;
                state.state3 = state.state2;
                state.state2 = output;

                outputBuffer[i] = output;
            }
        }
    }

    // Direct form 1. Apply two biquad filters in series and mix the result into the output buffer.
    static void ProcessDoubleBiquadFilterAndMix(BiquadFilterParameter2& parameter0, BiquadFilterParameter2& parameter1,
                                                BiquadFilterState& state0, BiquadFilterState& state1, Span<float> outputBuffer,
                                                Span<const float> inputBuffer, uint32_t sampleCount, float volume) {
        float a00 = parameter0.numerator[0];
        float a10 = parameter0.numerator[1];
        float a20 = parameter0.numerator[2];

        float b10 = parameter0.denominator[0];
        float b20 = parameter0.denominator[1];

        float a01 = parameter1.numerator[0];
        float a11 = parameter1.numerator[1];
        float a21 = parameter1.numerator[2];

        float b11 = parameter1.denominator[0];
        float b21 = parameter1.denominator[1];

        for (uint32_t i = 0; i < sampleCount; i++) {
            float input = inputBuffer[i];
            float output = input * a00 + state0.state0 * a10 + state0.state1 * a20 + state0.state2 * b10 + state0.state3 * b20;

            state0.state1 = state0.state0;
            state0.state0 = input;
            state0.state3 = state0.state2;
            state0.state2 = output;

            input = output;
            output = input * a01 + state1.state0 * a11 + state1.state1 * a21 + state1.state2 * b11 + state1.state3 * b21;

            state1.state1 = state1.state0;
            state1.state0 = input;
            state1.state3 = state1.state2;
            state1.state2 = output;

            outputBuffer[i] += FloatingPointMultiplyRoundUp(output, volume);
        }
    }

    // Direct form 1. As ProcessDoubleBiquadFilterAndMix with a volume ramp. Returns the last mixed sample value.
    static float ProcessDoubleBiquadFilterAndMixRamp(BiquadFilterParameter2& parameter0, BiquadFilterParameter2& parameter1,
                                                     BiquadFilterState& state0, BiquadFilterState& state1, Span<float> outputBuffer,
                                                     Span<const float> inputBuffer, uint32_t sampleCount, float volume, float ramp) {
        float a00 = parameter0.numerator[0];
        float a10 = parameter0.numerator[1];
        float a20 = parameter0.numerator[2];

        float b10 = parameter0.denominator[0];
        float b20 = parameter0.denominator[1];

        float a01 = parameter1.numerator[0];
        float a11 = parameter1.numerator[1];
        float a21 = parameter1.numerator[2];

        float b11 = parameter1.denominator[0];
        float b21 = parameter1.denominator[1];

        float mixState = 0.0f;

        for (uint32_t i = 0; i < sampleCount; i++) {
            float input = inputBuffer[i];
            float output = input * a00 + state0.state0 * a10 + state0.state1 * a20 + state0.state2 * b10 + state0.state3 * b20;

            state0.state1 = state0.state0;
            state0.state0 = input;
            state0.state3 = state0.state2;
            state0.state2 = output;

            input = output;
            output = input * a01 + state1.state0 * a11 + state1.state1 * a21 + state1.state2 * b11 + state1.state3 * b21;

            state1.state1 = state1.state0;
            state1.state0 = input;
            state1.state3 = state1.state2;
            state1.state2 = output;

            mixState = FloatingPointMultiplyRoundUp(output, volume);

            outputBuffer[i] += mixState;
            volume += ramp;
        }

        return mixState;
    }
};

}  // namespace nxrt::audren
