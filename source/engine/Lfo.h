#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace asampler
{
    // Per-pad low-frequency oscillator shape. Append-only: serialised by
    // AudioParameterChoice index, so reordering corrupts saved state.
    enum class LfoShape : int
    {
        Sine     = 0,
        Triangle = 1,
        Square   = 2,
        Saw      = 3    // rising ramp
    };

    // Where a pad's LFO routes. Append-only (AudioParameterChoice index).
    // Off is the default so legacy kits behave exactly as before.
    enum class LfoDest : int
    {
        Off    = 0,
        Pitch  = 1,   // vibrato / wobble  (±1 octave at full depth)
        Cutoff = 2,   // filter sweep      (±4 octaves at full depth)
        Volume = 3,   // tremolo           (down to silence at full depth)
        Pan    = 4     // auto-pan          (±full width at full depth)
    };

    // Evaluate an LFO shape at phase `phase01` in [0, 1). Returns [-1, 1].
    inline float lfoValue (LfoShape shape, double phase01) noexcept
    {
        const double p = phase01 - std::floor (phase01);   // wrap into [0, 1)

        switch (shape)
        {
            case LfoShape::Sine:
                return static_cast<float> (std::sin (p * juce::MathConstants<double>::twoPi));

            case LfoShape::Triangle:
            {
                // Starts at 0, rises to +1 at a quarter phase, down to -1 at
                // three-quarters, back to 0 — a symmetric triangle.
                double t;
                if (p < 0.25)      t = 4.0 * p;              //  0 -> +1
                else if (p < 0.75) t = 2.0 - 4.0 * p;        // +1 -> -1
                else               t = 4.0 * p - 4.0;        // -1 ->  0
                return static_cast<float> (t);
            }

            case LfoShape::Square:
                return (p < 0.5) ? 1.0f : -1.0f;

            case LfoShape::Saw:
                return static_cast<float> (2.0 * p - 1.0);
        }

        return 0.0f;
    }
}
