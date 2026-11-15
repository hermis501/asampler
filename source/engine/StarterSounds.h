#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace asampler
{
    // A small drum kit synthesised in code, so a new user has something to
    // play before finding samples of their own. Deterministic (fixed noise
    // seeds), mono, peaking at -1 dBFS.
    namespace starter
    {
        // Kick, Closed Hat, Open Hat, Snare, Clap, Rim, Tom, 808, in pad
        // order. The hats sit on pads 2 and 3, the choke pair every factory
        // kit uses.
        juce::StringArray soundNames();

        // The sound at `index` (0-based, in soundNames() order) at `sampleRate`.
        // An empty buffer for an index out of range.
        juce::AudioBuffer<float> render (int index, double sampleRate);
    }
}
