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

    // What the mod wheel (CC 1) drives on a pad. Append-only (choice index).
    // The direct destinations use the same full-scale ranges as the LFO; the
    // LFO depth destinations add the wheel to that LFO's depth, the classic
    // "wheel brings in vibrato".
    enum class ModDest : int
    {
        Off       = 0,
        Pitch     = 1,   // ±1 octave at full amount
        Cutoff    = 2,   // ±4 octaves
        Volume    = 3,   // down to silence (negative amount) or up to double
        Pan       = 4,   // ±full width
        Lfo1Depth = 5,
        Lfo2Depth = 6
    };

    // LFO tempo sync. Off runs at the Rate parameter in Hz; anything else
    // locks one LFO cycle to that note length at the host tempo.
    // Append-only (choice index).
    enum class LfoSync : int
    {
        Off = 0,
        FourBars, TwoBars, OneBar, Half, Quarter, Eighth, Sixteenth, ThirtySecond,
        HalfTriplet, QuarterTriplet, EighthTriplet, SixteenthTriplet,
        HalfDotted, QuarterDotted, EighthDotted
    };

    // Length of one LFO cycle in quarter-note beats, or 0 for Off.
    inline double lfoSyncBeats (LfoSync sync) noexcept
    {
        switch (sync)
        {
            case LfoSync::Off:              return 0.0;
            case LfoSync::FourBars:         return 16.0;
            case LfoSync::TwoBars:          return 8.0;
            case LfoSync::OneBar:           return 4.0;
            case LfoSync::Half:             return 2.0;
            case LfoSync::Quarter:          return 1.0;
            case LfoSync::Eighth:           return 0.5;
            case LfoSync::Sixteenth:        return 0.25;
            case LfoSync::ThirtySecond:     return 0.125;
            case LfoSync::HalfTriplet:      return 4.0 / 3.0;
            case LfoSync::QuarterTriplet:   return 2.0 / 3.0;
            case LfoSync::EighthTriplet:    return 1.0 / 3.0;
            case LfoSync::SixteenthTriplet: return 1.0 / 6.0;
            case LfoSync::HalfDotted:       return 3.0;
            case LfoSync::QuarterDotted:    return 1.5;
            case LfoSync::EighthDotted:     return 0.75;
        }
        return 0.0;
    }

    // The rate an LFO should run at: the free Rate in Hz, or the synced note
    // length at `bpm`.
    inline float lfoRateFor (float freeRateHz, LfoSync sync, double bpm) noexcept
    {
        const double beats = lfoSyncBeats (sync);
        if (beats <= 0.0 || bpm <= 0.0)
            return freeRateHz;
        return static_cast<float> ((bpm / 60.0) / beats);
    }

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
