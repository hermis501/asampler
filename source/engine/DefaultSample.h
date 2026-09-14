#pragma once

#include "Sample.h"

#include <cmath>

namespace asampler
{
    // Built-in test tone used by every pad until the user loads a real sample.
    // Decaying A4 sine plucked at 60% peak amplitude — recognisable, not loud,
    // dies out on its own so a single tap doesn't ring forever.
    inline Sample generateDefaultSample (double sampleRate,
                                         float freqHz       = 440.0f,
                                         float durationSec  = 0.4f,
                                         float peakAmp      = 0.6f,
                                         float decayRate    = 3.5f)
    {
        const int numSamples = juce::jmax (1, static_cast<int> (sampleRate * durationSec));
        juce::AudioBuffer<float> buf (2, numSamples);

        const float twoPiFreq = juce::MathConstants<float>::twoPi * freqHz;

        for (int i = 0; i < numSamples; ++i)
        {
            const float t   = static_cast<float> (i) / static_cast<float> (sampleRate);
            const float env = std::exp (-decayRate * t);
            const float s   = std::sin (twoPiFreq * t) * env * peakAmp;
            buf.setSample (0, i, s);
            buf.setSample (1, i, s);
        }

        return Sample (std::move (buf), sampleRate);
    }
}
