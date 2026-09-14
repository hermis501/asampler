#include "PhaseVocoder.h"
#include "Transient.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace asampler
{
    namespace
    {
        // 2048 at 44.1 kHz is ~46 ms: long enough to resolve bass partials,
        // short enough that transient smearing stays correctable. This is the
        // usual operating point for music-grade stretching.
        constexpr int kFftOrder = 11;
        constexpr int kFftSize  = 1 << kFftOrder;   // 2048
        constexpr int kBins     = kFftSize / 2 + 1;
        constexpr int kHopA     = kFftSize / 4;     // 75% overlap

        // Cepstral coefficients kept when estimating the spectral envelope.
        // Higher keeps more detail (and starts tracking individual harmonics,
        // which defeats the point); lower over-smooths and loses the formants.
        constexpr int kLiftOrder = 48;

        constexpr float kEps = 1.0e-9f;

        // Wrap a phase difference into (-pi, pi].
        inline float principalArgument (float phase) noexcept
        {
            constexpr float twoPi = 2.0f * juce::MathConstants<float>::pi;
            return phase - twoPi * std::floor (phase / twoPi + 0.5f);
        }

        juce::AudioBuffer<float> copyOf (const juce::AudioBuffer<float>& in)
        {
            juce::AudioBuffer<float> out (in.getNumChannels(), in.getNumSamples());
            for (int ch = 0; ch < in.getNumChannels(); ++ch)
                out.copyFrom (ch, 0, in, ch, 0, in.getNumSamples());
            return out;
        }
    }

    bool isNeutral (const StretchOptions& options) noexcept
    {
        return std::abs (options.timeRatio - 1.0) < 1.0e-6
            && std::abs (options.formantSemitones) < 1.0e-6;
    }

    std::vector<float> computeSpectralEnvelope (const std::vector<float>& magnitudes,
                                                int liftOrder)
    {
        const int bins = static_cast<int> (magnitudes.size());
        if (bins <= 1)
            return magnitudes;

        // The cepstrum needs a full symmetric spectrum, so mirror the
        // half-spectrum we were given back out to fftSize.
        const int fftSize = (bins - 1) * 2;
        juce::dsp::FFT fft (static_cast<int> (std::log2 (fftSize)));

        std::vector<juce::dsp::Complex<float>> work ((std::size_t) fftSize);
        std::vector<juce::dsp::Complex<float>> spec ((std::size_t) fftSize);

        // Log magnitude — the domain where a multiplicative envelope becomes
        // an additive low-quefrency component we can simply cut out.
        for (int k = 0; k < fftSize; ++k)
        {
            const int mirrored = (k < bins) ? k : (fftSize - k);
            const float mag = magnitudes[(std::size_t) juce::jlimit (0, bins - 1, mirrored)];
            work[(std::size_t) k] = { std::log (mag + kEps), 0.0f };
        }

        fft.perform (work.data(), spec.data(), /*inverse*/ true);   // -> cepstrum

        // Lifter: keep low quefrencies (the smooth envelope), discard the rest
        // (the harmonic fine structure).
        const int keep = juce::jlimit (1, fftSize / 2, liftOrder);
        for (int q = 0; q < fftSize; ++q)
        {
            const int distanceFromDc = juce::jmin (q, fftSize - q);
            if (distanceFromDc > keep)
                spec[(std::size_t) q] = { 0.0f, 0.0f };
        }

        fft.perform (spec.data(), work.data(), /*inverse*/ false);  // -> log envelope

        std::vector<float> envelope ((std::size_t) bins);
        for (int k = 0; k < bins; ++k)
            envelope[(std::size_t) k] = std::exp (work[(std::size_t) k].real());

        return envelope;
    }

    juce::AudioBuffer<float> processOffline (const juce::AudioBuffer<float>& input,
                                             double sampleRate,
                                             const StretchOptions& options)
    {
        const int numChannels = input.getNumChannels();
        const int numSamples  = input.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return copyOf (input);

        if (isNeutral (options))
            return copyOf (input);

        // Too short to analyse — stretching it would produce nothing useful.
        if (numSamples < kFftSize)
            return copyOf (input);

        const double timeRatio = juce::jlimit (0.25, 4.0, options.timeRatio);
        const double formantWarp = std::pow (2.0, juce::jlimit (-24.0, 24.0,
                                                                options.formantSemitones) / 12.0);
        const bool warpFormants = std::abs (formantWarp - 1.0) > 1.0e-6;

        const int hopS = juce::jmax (1, juce::roundToInt (kHopA * timeRatio));

        // Onset positions, so synthesis phase can be reset where the material
        // actually has an attack. Computed once on the mono sum.
        std::vector<int> onsets;
        if (options.preserveTransients)
        {
            const auto hits = detectTransients (input, sampleRate, OnsetMethod::LogEnergy);
            onsets.reserve (hits.size());
            for (const auto& h : hits)
                onsets.push_back (h.sampleIndex);
            std::sort (onsets.begin(), onsets.end());
        }

        const int numFrames = (numSamples - kFftSize) / kHopA + 1;
        const int outLength = juce::jmax (1, (numFrames - 1) * hopS + kFftSize);

        juce::AudioBuffer<float> output (numChannels, outLength);
        output.clear();

        // Accumulated window energy per output sample. Dividing by this at the
        // end makes the overlap-add correct for any hop size, rather than only
        // for the hops where the window happens to sum to a constant.
        std::vector<float> windowSum ((std::size_t) outLength, 0.0f);

        // Periodic Hann, applied on both analysis and synthesis.
        std::vector<float> window ((std::size_t) kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            window[(std::size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                              * static_cast<float> (i)
                                                              / static_cast<float> (kFftSize));

        juce::dsp::FFT fft (kFftOrder);

        std::vector<juce::dsp::Complex<float>> timeIn  ((std::size_t) kFftSize);
        std::vector<juce::dsp::Complex<float>> freqOut ((std::size_t) kFftSize);
        std::vector<juce::dsp::Complex<float>> freqIn  ((std::size_t) kFftSize);
        std::vector<juce::dsp::Complex<float>> timeOut ((std::size_t) kFftSize);

        std::vector<float> magnitude ((std::size_t) kBins);
        std::vector<float> phase     ((std::size_t) kBins);
        std::vector<float> prevPhase ((std::size_t) kBins);
        std::vector<float> sumPhase  ((std::size_t) kBins);
        std::vector<float> synthPhase((std::size_t) kBins);
        std::vector<int>   peaks;

        // Bin centre frequency, in radians per analysis hop.
        std::vector<float> omega ((std::size_t) kBins);
        for (int k = 0; k < kBins; ++k)
            omega[(std::size_t) k] = 2.0f * juce::MathConstants<float>::pi
                                   * static_cast<float> (k) / static_cast<float> (kFftSize);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            std::fill (prevPhase.begin(), prevPhase.end(), 0.0f);
            std::fill (sumPhase.begin(),  sumPhase.end(),  0.0f);

            const float* src = input.getReadPointer (ch);
            float*       dst = output.getWritePointer (ch);

            std::size_t nextOnset = 0;

            for (int frame = 0; frame < numFrames; ++frame)
            {
                const int inPos  = frame * kHopA;
                const int outPos = frame * hopS;

                // Does an onset fall inside this analysis frame's first hop?
                bool frameHasOnset = false;
                if (options.preserveTransients)
                {
                    while (nextOnset < onsets.size() && onsets[nextOnset] < inPos)
                        ++nextOnset;
                    if (nextOnset < onsets.size() && onsets[nextOnset] < inPos + kHopA)
                    {
                        frameHasOnset = true;
                        ++nextOnset;
                    }
                }

                for (int i = 0; i < kFftSize; ++i)
                    timeIn[(std::size_t) i] = { src[inPos + i] * window[(std::size_t) i], 0.0f };

                fft.perform (timeIn.data(), freqOut.data(), false);

                for (int k = 0; k < kBins; ++k)
                {
                    const auto c = freqOut[(std::size_t) k];
                    magnitude[(std::size_t) k] = std::abs (c);
                    phase    [(std::size_t) k] = std::arg (c);
                }

                // ---- Formant warp -------------------------------------------------
                // Divide out the spectral envelope, resample the envelope along
                // the frequency axis, and multiply it back. The harmonics keep
                // their positions (pitch unchanged) while the resonances move.
                if (warpFormants)
                {
                    const auto envelope = computeSpectralEnvelope (magnitude, kLiftOrder);

                    for (int k = 0; k < kBins; ++k)
                    {
                        // Where this bin's new envelope value comes from.
                        const double srcBin = static_cast<double> (k) / formantWarp;

                        float warped = 0.0f;
                        if (srcBin >= 0.0 && srcBin <= kBins - 1)
                        {
                            const int   i0 = static_cast<int> (srcBin);
                            const int   i1 = juce::jmin (i0 + 1, kBins - 1);
                            const float fr = static_cast<float> (srcBin - i0);
                            warped = envelope[(std::size_t) i0] * (1.0f - fr)
                                   + envelope[(std::size_t) i1] * fr;
                        }

                        const float original = envelope[(std::size_t) k];
                        if (original > kEps)
                            magnitude[(std::size_t) k] *= warped / original;
                        else
                            magnitude[(std::size_t) k] = 0.0f;
                    }
                }

                // ---- Phase propagation --------------------------------------------
                if (frame == 0 || frameHasOnset)
                {
                    // Start of the material, or an attack: take the analysis
                    // phase verbatim. Propagating through an onset is exactly
                    // what smears it.
                    for (int k = 0; k < kBins; ++k)
                        sumPhase[(std::size_t) k] = phase[(std::size_t) k];
                }
                else
                {
                    for (int k = 0; k < kBins; ++k)
                    {
                        // Deviation from this bin's expected phase advance
                        // gives the partial's true frequency.
                        const float expected = omega[(std::size_t) k] * static_cast<float> (kHopA);
                        const float delta    = principalArgument (
                            phase[(std::size_t) k] - prevPhase[(std::size_t) k] - expected);
                        const float trueFreq = omega[(std::size_t) k]
                                             + delta / static_cast<float> (kHopA);

                        sumPhase[(std::size_t) k] += trueFreq * static_cast<float> (hopS);
                    }
                }

                std::copy (phase.begin(), phase.end(), prevPhase.begin());

                // ---- Phase locking -------------------------------------------------
                // Rotate each spectral peak's neighbourhood rigidly by that
                // peak's advance, instead of letting every bin drift on its
                // own. This is what keeps partials coherent and removes the
                // watery "phasiness" of a plain vocoder.
                synthPhase = sumPhase;

                peaks.clear();
                for (int k = 2; k < kBins - 2; ++k)
                {
                    const float m = magnitude[(std::size_t) k];
                    if (m > magnitude[(std::size_t) (k - 1)] && m > magnitude[(std::size_t) (k + 1)]
                        && m > magnitude[(std::size_t) (k - 2)] && m > magnitude[(std::size_t) (k + 2)])
                        peaks.push_back (k);
                }

                if (! peaks.empty())
                {
                    for (std::size_t p = 0; p < peaks.size(); ++p)
                    {
                        const int peak = peaks[p];

                        // Region of influence: up to the midpoint between this
                        // peak and its neighbours.
                        const int lo = (p == 0)
                                         ? 0
                                         : (peaks[p - 1] + peak) / 2 + 1;
                        const int hi = (p + 1 == peaks.size())
                                         ? kBins - 1
                                         : (peak + peaks[p + 1]) / 2;

                        const float rotation = sumPhase[(std::size_t) peak]
                                             - phase[(std::size_t) peak];

                        for (int k = lo; k <= hi; ++k)
                            synthPhase[(std::size_t) k] = phase[(std::size_t) k] + rotation;
                    }
                }

                // ---- Resynthesis ---------------------------------------------------
                for (int k = 0; k < kBins; ++k)
                {
                    const float m = magnitude[(std::size_t) k];
                    const float p = synthPhase[(std::size_t) k];
                    const juce::dsp::Complex<float> c { m * std::cos (p), m * std::sin (p) };

                    freqIn[(std::size_t) k] = c;

                    // Hermitian symmetry for the negative frequencies, so the
                    // inverse transform comes back real.
                    if (k > 0 && k < kFftSize - k)
                        freqIn[(std::size_t) (kFftSize - k)] = std::conj (c);
                }

                fft.perform (freqIn.data(), timeOut.data(), true);

                for (int i = 0; i < kFftSize; ++i)
                {
                    const int at = outPos + i;
                    if (at >= outLength)
                        break;

                    const float w = window[(std::size_t) i];
                    dst[at] += timeOut[(std::size_t) i].real() * w;

                    // Only accumulate the window sum once, not per channel.
                    if (ch == 0)
                        windowSum[(std::size_t) at] += w * w;
                }
            }
        }

        // Normalise by the accumulated window energy.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* dst = output.getWritePointer (ch);
            for (int i = 0; i < outLength; ++i)
            {
                const float w = windowSum[(std::size_t) i];
                if (w > 1.0e-6f)
                    dst[i] /= w;
            }
        }

        // Trim to the length the requested ratio implies, so a 2x stretch
        // really is twice as long rather than "twice plus a window".
        const int wanted = juce::jlimit (1, outLength,
                                         juce::roundToInt (numSamples * timeRatio));

        juce::AudioBuffer<float> trimmed (numChannels, wanted);
        for (int ch = 0; ch < numChannels; ++ch)
            trimmed.copyFrom (ch, 0, output, ch, 0, wanted);

        // Guard against a pathological render blowing up the level. Phase
        // locking keeps this well behaved in practice, but a single clipped
        // pad is a bad way to find out otherwise.
        const auto peak = trimmed.getMagnitude (0, wanted);
        if (peak > 1.0e-6f)
        {
            const auto inputPeak = input.getMagnitude (0, numSamples);
            if (inputPeak > 1.0e-6f && peak > inputPeak * 1.05f)
                trimmed.applyGain (inputPeak / peak);
        }

        return trimmed;
    }
}
