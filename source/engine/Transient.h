#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace asampler
{
    struct TransientHit
    {
        int   sampleIndex;  // position in the source audio buffer (0-based)
        float strength;     // relative onset strength (peak novelty value)
    };

    // Which novelty function the detector should build.
    //
    // LogEnergy is the default — log of per-frame energy, half-wave-rectified
    // difference. Cheap, robust for percussive content (drums, plucks, sharp
    // attacks). Tends to miss soft attacks (pads, vocals, bowed strings)
    // because energy doesn't rise sharply on those.
    //
    // SpectralFlux runs an FFT per frame and sums the half-wave-rectified
    // bin-by-bin magnitude difference between consecutive frames. Catches
    // timbral changes (a vocal entering, a chord change) even when the
    // overall energy is steady — which is the case the energy detector
    // misses. Roughly 5-10x slower because of the per-frame FFT, but only
    // runs during user-triggered chop, so the cost is invisible.
    enum class OnsetMethod : int
    {
        LogEnergy    = 0,
        SpectralFlux = 1
    };

    // Onset detection used by the sampler's transient-mode auto-chop.
    //
    // Returns hits sorted ascending by `sampleIndex`. Empty audio, audio
    // shorter than the analysis window, or audio without sufficient novelty
    // returns an empty vector — the caller decides how to fall back.
    std::vector<TransientHit> detectTransients (const juce::AudioBuffer<float>& audio,
                                                double sampleRate,
                                                OnsetMethod method = OnsetMethod::LogEnergy);
}
