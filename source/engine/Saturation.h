#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace asampler
{
    // Per-pad "character" saturation. Append-only: the value index is
    // serialised by AudioParameterChoice, so reordering corrupts saved state.
    enum class SaturationMode : int
    {
        Tube     = 0,   // asymmetric soft clip — adds even harmonics, warm
        Tape     = 1,   // symmetric soft clip — gentle rounding / glue
        HardFold = 2    // aggressive drive into a hard clip — gritty, trap 808s
    };

    // Drive one sample through the chosen saturation curve.
    //
    // `amount` in [0, 1] sets the drive: 0 is a true bypass (returns the input
    // untouched), 1 is maximum grit. Each curve is roughly unity-gain at the
    // extremes so raising drive fattens the tone without a huge level jump —
    // the caller does not need to compensate. Output is bounded to about
    // [-1, 1] for Tube/Tape (soft) and exactly [-1, 1] for HardFold.
    inline float applySaturation (float x, SaturationMode mode, float amount) noexcept
    {
        if (amount <= 0.0f)
            return x;

        const float a = juce::jlimit (0.0f, 1.0f, amount);

        switch (mode)
        {
            case SaturationMode::Tube:
            {
                // Asymmetric: harder curve on the positive half generates the
                // even-order harmonics that read as "tube warmth".
                const float g    = 1.0f + a * 9.0f;              // 1..10
                const float norm = std::tanh (g);
                const float bias = (x >= 0.0f) ? g : g * 0.7f;
                return std::tanh (bias * x) / norm;
            }

            case SaturationMode::Tape:
            {
                // Symmetric soft clip — the classic tanh transfer, gentle knee.
                const float g = 1.0f + a * 6.0f;                 // 1..7
                return std::tanh (g * x) / std::tanh (g);
            }

            case SaturationMode::HardFold:
            {
                // Drive hard, then clip. At high amounts this squares off the
                // wave for that lo-fi/distorted 808 edge.
                const float g = 1.0f + a * 24.0f;                // 1..25
                return juce::jlimit (-1.0f, 1.0f, g * x);
            }
        }

        return x;
    }

    // Quantise one sample to `bits` of resolution. `bits` >= 16 is a bypass
    // (returns the input untouched); lower values step the signal onto a
    // coarser grid for that 12-bit SP-1200 / MPC60 grit. Clamped to a sane
    // floor so a stray tiny value can't blow up the step size.
    inline float applyBitDepthReduction (float x, int bits) noexcept
    {
        if (bits >= 16)
            return x;

        const int   safeBits = juce::jlimit (2, 15, bits);
        const float levels   = static_cast<float> (1 << (safeBits - 1));
        const float step     = 1.0f / levels;
        return std::round (x / step) * step;
    }
}
