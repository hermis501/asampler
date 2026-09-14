#include "Transient.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace asampler
{
    namespace
    {
        // Hop and analysis-window sizes chosen for ~5.8 ms frame resolution at
        // 44.1 kHz and ~11.6 ms windows. Smaller windows localise onsets more
        // tightly (a 1024-sample window can report an onset up to ~23 ms before
        // the actual attack just because the window straddles it); 512 keeps
        // the energy estimate stable while keeping the reported sampleIndex
        // close to the real attack.
        constexpr int   kHop         = 256;
        constexpr int   kWindow      = 512;
        constexpr float kEnergyFloor = 1.0e-10f;

        // FFT for spectral flux. Order 10 = 1024 samples, ~23 ms at 44.1 kHz,
        // giving ~43 Hz bin spacing. Wider than kWindow so we get enough
        // frequency resolution for the flux signal to be meaningful; the
        // hop is still kHop so peak localisation matches the energy detector.
        constexpr int   kFftOrder    = 10;
        constexpr int   kFftSize     = 1 << kFftOrder;
        constexpr int   kFftBins     = kFftSize / 2;

        // Adaptive-threshold scale: novelty must clear (mean + scale*std) to
        // qualify as a peak. 1.5 picks up clear hits while suppressing
        // background ripples on quiet content.
        constexpr float kThresholdScale = 1.5f;

        // Minimum spacing between detected hits (in seconds). Drum hits
        // realistically don't repeat faster than this even on fast rolls;
        // anything closer is almost certainly the decay of the previous hit.
        constexpr double kMinGapSeconds = 0.04;

        // Mean + std-dev threshold and local-max peak pick with min-gap.
        // Shared between the two novelty functions — the only thing that
        // differs between detector variants is how the novelty[] vector is
        // built, so picking is factored out here.
        std::vector<TransientHit> pickPeaks (const std::vector<float>& novelty,
                                             int hop,
                                             double sampleRate)
        {
            const int numFrames = static_cast<int> (novelty.size());
            if (numFrames < 3) return {};

            float maxNovelty = 0.0f;
            for (float v : novelty) if (v > maxNovelty) maxNovelty = v;
            if (maxNovelty <= 0.0f) return {};

            double mean = 0.0;
            for (float v : novelty) mean += v;
            mean /= static_cast<double> (numFrames);

            double variance = 0.0;
            for (float v : novelty)
            {
                const double d = static_cast<double> (v) - mean;
                variance += d * d;
            }
            variance /= static_cast<double> (numFrames);
            const float stddev    = std::sqrt (static_cast<float> (variance));
            const float threshold = static_cast<float> (mean) + kThresholdScale * stddev;

            const int minGapFrames = std::max (1,
                static_cast<int> ((kMinGapSeconds * sampleRate) / hop));

            std::vector<TransientHit> hits;
            int   lastPeakFrame    = -minGapFrames - 1;
            float lastPeakStrength = 0.0f;

            for (int f = 1; f < numFrames - 1; ++f)
            {
                const float v = novelty[(std::size_t) f];
                if (v < threshold) continue;
                if (v <= novelty[(std::size_t) (f - 1)]) continue;
                if (v <  novelty[(std::size_t) (f + 1)]) continue;

                if ((f - lastPeakFrame) >= minGapFrames)
                {
                    hits.push_back ({ f * hop, v });
                    lastPeakFrame    = f;
                    lastPeakStrength = v;
                }
                else if (v > lastPeakStrength)
                {
                    hits.back() = { f * hop, v };
                    lastPeakFrame    = f;
                    lastPeakStrength = v;
                }
            }

            return hits;
        }

        std::vector<float> buildLogEnergyNovelty (const juce::AudioBuffer<float>& audio)
        {
            const int numSamples  = audio.getNumSamples();
            const int numChannels = audio.getNumChannels();

            if (numSamples < kWindow || numChannels <= 0)
                return {};

            const int numFrames = (numSamples - kWindow) / kHop + 1;
            if (numFrames < 3) return {};

            std::vector<float> energy (static_cast<std::size_t> (numFrames));
            for (int f = 0; f < numFrames; ++f)
            {
                double sumSq = 0.0;
                const int start = f * kHop;
                for (int n = 0; n < kWindow; ++n)
                {
                    float mono = 0.0f;
                    for (int ch = 0; ch < numChannels; ++ch)
                        mono += audio.getSample (ch, start + n);
                    mono /= static_cast<float> (numChannels);
                    sumSq += static_cast<double> (mono) * static_cast<double> (mono);
                }
                energy[(std::size_t) f] = std::log (static_cast<float> (sumSq) + kEnergyFloor);
            }

            std::vector<float> novelty (static_cast<std::size_t> (numFrames), 0.0f);
            for (int f = 1; f < numFrames; ++f)
            {
                const float d = energy[(std::size_t) f] - energy[(std::size_t) (f - 1)];
                novelty[(std::size_t) f] = d > 0.0f ? d : 0.0f;
            }
            return novelty;
        }

        std::vector<float> buildSpectralFluxNovelty (const juce::AudioBuffer<float>& audio)
        {
            const int numSamples  = audio.getNumSamples();
            const int numChannels = audio.getNumChannels();

            if (numSamples < kFftSize || numChannels <= 0)
                return {};

            const int numFrames = (numSamples - kFftSize) / kHop + 1;
            if (numFrames < 3) return {};

            // Build a mono mix in one pass so the FFT loop doesn't keep
            // re-walking the multi-channel source. Stays in float to match
            // the FFT input contract.
            std::vector<float> mono (static_cast<std::size_t> (numSamples), 0.0f);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto* r = audio.getReadPointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    mono[(std::size_t) i] += r[i];
            }
            if (numChannels > 1)
            {
                const float scale = 1.0f / static_cast<float> (numChannels);
                for (auto& v : mono) v *= scale;
            }

            // Hann window — precomputed once and reused per frame.
            std::array<float, kFftSize> window {};
            for (int i = 0; i < kFftSize; ++i)
                window[(std::size_t) i] = 0.5f * (1.0f - std::cos (
                    juce::MathConstants<float>::twoPi
                        * static_cast<float> (i)
                        / static_cast<float> (kFftSize - 1)));

            juce::dsp::FFT fft (kFftOrder);
            // performFrequencyOnlyForwardTransform needs 2*fftSize floats:
            // input lives in the first fftSize slots, the bin magnitudes get
            // written back into those same first fftSize slots.
            std::vector<float> work (static_cast<std::size_t> (kFftSize) * 2, 0.0f);
            std::vector<float> prevMag (kFftBins, 0.0f);
            std::vector<float> novelty (static_cast<std::size_t> (numFrames), 0.0f);

            for (int f = 0; f < numFrames; ++f)
            {
                const int start = f * kHop;
                for (int i = 0; i < kFftSize; ++i)
                    work[(std::size_t) i] = mono[(std::size_t) (start + i)]
                                              * window[(std::size_t) i];
                std::fill (work.begin() + kFftSize, work.end(), 0.0f);

                fft.performFrequencyOnlyForwardTransform (work.data());

                float flux = 0.0f;
                for (int b = 0; b < kFftBins; ++b)
                {
                    const float diff = work[(std::size_t) b] - prevMag[(std::size_t) b];
                    if (diff > 0.0f) flux += diff;
                }
                novelty[(std::size_t) f] = (f == 0) ? 0.0f : flux;
                std::copy (work.begin(), work.begin() + kFftBins, prevMag.begin());
            }
            return novelty;
        }
    }

    std::vector<TransientHit> detectTransients (const juce::AudioBuffer<float>& audio,
                                                double sampleRate,
                                                OnsetMethod method)
    {
        if (method == OnsetMethod::SpectralFlux)
            return pickPeaks (buildSpectralFluxNovelty (audio), kHop, sampleRate);
        return pickPeaks (buildLogEnergyNovelty (audio), kHop, sampleRate);
    }
}
