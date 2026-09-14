#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace asampler
{
    // A loaded audio sample: an interleaved-by-channel buffer plus the
    // sample rate it was recorded at. Voices interpolate over this when
    // playing back at a different host rate or a different pitch.
    class Sample
    {
    public:
        Sample() = default;

        Sample (juce::AudioBuffer<float> data, double sourceSampleRate)
            : buffer (std::move (data)), sampleRate (sourceSampleRate)
        {
        }

        bool isEmpty()                  const noexcept { return buffer.getNumSamples() == 0; }
        int  getNumChannels()           const noexcept { return buffer.getNumChannels(); }
        int  getNumSamples()            const noexcept { return buffer.getNumSamples(); }
        double getSourceSampleRate()    const noexcept { return sampleRate; }
        const float* getReadPointer (int ch) const noexcept { return buffer.getReadPointer (ch); }

    private:
        juce::AudioBuffer<float> buffer;
        double sampleRate = 44100.0;
    };
}
