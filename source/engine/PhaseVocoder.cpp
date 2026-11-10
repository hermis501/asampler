#include "PhaseVocoder.h"
#include "Transient.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace asampler
{
    namespace
    {
        // ~46 ms of audio per analysis window: long enough to resolve bass
        // partials, short enough that transient smearing stays correctable.
        // That is 2048 samples at 44.1/48 kHz, the usual operating point for
        // music-grade stretching. The size scales with the sample rate so a
        // 96 kHz sample gets the same time span (and the same bass
        // resolution) rather than half of it.
        int fftOrderFor (double sampleRate) noexcept
        {
            if (sampleRate > 132000.0) return 13;   // 176.4 / 192 kHz
            if (sampleRate >  66000.0) return 12;   // 88.2 / 96 kHz
            return 11;
        }

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

        using Complex = juce::dsp::Complex<float>;

        // Reusable buffers for envelope extraction. Building a juce::dsp::FFT
        // computes its twiddle tables, so doing that per frame per channel
        // (as a naive call would) costs more than the transforms themselves.
        struct EnvelopeWork
        {
            explicit EnvelopeWork (int fftOrder)
                : fft (fftOrder), size (1 << fftOrder),
                  a ((std::size_t) size), b ((std::size_t) size) {}

            juce::dsp::FFT       fft;
            int                  size;
            std::vector<Complex> a, b;
        };

        // Spectral envelopes of up to two channels in one pass. A log
        // magnitude spectrum mirrored out to full length is real and even, so
        // its transform is real too: packing channel A into the real part and
        // channel B into the imaginary part gets both cepstra from one
        // transform, and both envelopes back from one more. `magB` / `envB`
        // may be null for a single channel.
        void spectralEnvelopes (const float* magA, const float* magB, int bins,
                                int liftOrder, float* envA, float* envB,
                                EnvelopeWork& w)
        {
            const int fftSize = w.size;

            for (int k = 0; k < fftSize; ++k)
            {
                const int mirrored = juce::jlimit (0, bins - 1, (k < bins) ? k : (fftSize - k));
                w.a[(std::size_t) k] = { std::log (magA[mirrored] + kEps),
                                         magB != nullptr ? std::log (magB[mirrored] + kEps) : 0.0f };
            }

            w.fft.perform (w.a.data(), w.b.data(), /*inverse*/ true);   // -> cepstra

            // Lifter: keep low quefrencies (the smooth envelope), discard the
            // rest (the harmonic fine structure).
            const int keep = juce::jlimit (1, fftSize / 2, liftOrder);
            for (int q = keep + 1; q < fftSize - keep; ++q)
                w.b[(std::size_t) q] = { 0.0f, 0.0f };

            w.fft.perform (w.b.data(), w.a.data(), /*inverse*/ false);  // -> log envelopes

            for (int k = 0; k < bins; ++k)
            {
                envA[k] = std::exp (w.a[(std::size_t) k].real());
                if (envB != nullptr)
                    envB[k] = std::exp (w.a[(std::size_t) k].imag());
            }
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

        // The cepstrum needs a full symmetric spectrum, so the half-spectrum
        // we were given is mirrored back out to fftSize.
        const int fftSize = (bins - 1) * 2;
        EnvelopeWork work (static_cast<int> (std::log2 (fftSize)));

        std::vector<float> envelope ((std::size_t) bins);
        spectralEnvelopes (magnitudes.data(), nullptr, bins, liftOrder,
                           envelope.data(), nullptr, work);
        return envelope;
    }

    juce::AudioBuffer<float> processOffline (const juce::AudioBuffer<float>& input,
                                             double sampleRate,
                                             const StretchOptions& options,
                                             const std::function<bool()>& shouldAbort)
    {
        const int numChannels = input.getNumChannels();
        const int numSamples  = input.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return copyOf (input);

        if (isNeutral (options))
            return copyOf (input);

        const int fftOrder = fftOrderFor (sampleRate);
        const int fftSize  = 1 << fftOrder;
        const int bins     = fftSize / 2 + 1;
        const int hopA     = fftSize / 4;          // 75% overlap

        // Too short to analyse — stretching it would produce nothing useful.
        if (numSamples < fftSize)
            return copyOf (input);

        const double timeRatio = juce::jlimit (0.25, 4.0, options.timeRatio);
        const double formantWarp = std::pow (2.0, juce::jlimit (-24.0, 24.0,
                                                                options.formantSemitones) / 12.0);
        const bool warpFormants = std::abs (formantWarp - 1.0) > 1.0e-6;

        const int hopS = juce::jmax (1, juce::roundToInt (hopA * timeRatio));

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

        const int numFrames = (numSamples - fftSize) / hopA + 1;
        const int outLength = juce::jmax (1, (numFrames - 1) * hopS + fftSize);

        juce::AudioBuffer<float> output (numChannels, outLength);
        output.clear();

        // Accumulated window energy per output sample. Dividing by this at the
        // end makes the overlap-add correct for any hop size, rather than only
        // for the hops where the window happens to sum to a constant.
        std::vector<float> windowSum ((std::size_t) outLength, 0.0f);

        // Periodic Hann, applied on both analysis and synthesis.
        std::vector<float> window ((std::size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(std::size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                              * static_cast<float> (i)
                                                              / static_cast<float> (fftSize));

        juce::dsp::FFT fft (fftOrder);
        EnvelopeWork envWork (fftOrder);

        std::vector<Complex> timeIn  ((std::size_t) fftSize);
        std::vector<Complex> freqIn  ((std::size_t) fftSize);
        std::vector<Complex> timeOut ((std::size_t) fftSize);
        std::vector<Complex> packed  ((std::size_t) fftSize);
        std::vector<float>   envelope ((std::size_t) bins * (std::size_t) numChannels);

        // Per-channel analysis of the current frame.
        std::vector<std::vector<Complex>> spectrum ((std::size_t) numChannels,
                                                    std::vector<Complex> ((std::size_t) fftSize));
        std::vector<std::vector<float>> magnitude ((std::size_t) numChannels,
                                                   std::vector<float> ((std::size_t) bins));
        std::vector<Complex> rotor ((std::size_t) bins);

        // ---- Stereo linking ----------------------------------------------------
        // Every channel shares one phase *rotation* per bin: the synthesis
        // phase is the channel's own analysis phase plus that rotation. The
        // phase difference between channels in each bin therefore comes out
        // exactly as it went in, which holds the stereo image and the mono sum
        // together. Tracking each channel on its own lets them drift apart
        // wherever the channels hear different things in the same bin (two
        // instruments panned apart) and the image smears.
        //
        // The rotation advances by trueFreq * (hopS - hopA) per frame — the
        // standard phase vocoder update, written as synthesis-minus-analysis
        // phase. Each bin's true frequency is estimated from the mid (sum)
        // signal, or from the side (difference) when that is louder, so
        // out-of-phase material doesn't feed it a near-silent, noisy phase.
        std::vector<float> rotation   ((std::size_t) bins, 0.0f);
        std::vector<float> locked     ((std::size_t) bins, 0.0f);
        std::vector<float> prevMid    ((std::size_t) bins, 0.0f);
        std::vector<float> prevSide   ((std::size_t) bins, 0.0f);
        std::vector<float> peakWeight ((std::size_t) bins, 0.0f);
        std::vector<int>   peaks;

        const bool haveSide = numChannels == 2;

        // Bin centre frequency, in radians per sample.
        std::vector<float> omega ((std::size_t) bins);
        for (int k = 0; k < bins; ++k)
            omega[(std::size_t) k] = 2.0f * juce::MathConstants<float>::pi
                                   * static_cast<float> (k) / static_cast<float> (fftSize);

        std::size_t nextOnset = 0;

        for (int frame = 0; frame < numFrames; ++frame)
        {
            // A superseded render stops early; the caller discards the result.
            if (shouldAbort && (frame & 31) == 0 && shouldAbort())
                return {};

            const int inPos  = frame * hopA;
            const int outPos = frame * hopS;

            // Does an onset fall inside this analysis frame's first hop?
            bool frameHasOnset = false;
            if (options.preserveTransients)
            {
                while (nextOnset < onsets.size() && onsets[nextOnset] < inPos)
                    ++nextOnset;
                if (nextOnset < onsets.size() && onsets[nextOnset] < inPos + hopA)
                {
                    frameHasOnset = true;
                    ++nextOnset;
                }
            }

            // ---- Analysis ----------------------------------------------------
            // Two real channels share one complex transform: channel A in the
            // real part, B in the imaginary part. The spectra separate again
            // by Hermitian symmetry, halving the analysis cost for stereo.
            for (int ch = 0; ch < numChannels; ch += 2)
            {
                const bool pair = ch + 1 < numChannels;
                const float* srcA = input.getReadPointer (ch);
                const float* srcB = pair ? input.getReadPointer (ch + 1) : nullptr;
                for (int i = 0; i < fftSize; ++i)
                {
                    const float w = window[(std::size_t) i];
                    timeIn[(std::size_t) i] = { srcA[inPos + i] * w,
                                                pair ? srcB[inPos + i] * w : 0.0f };
                }

                fft.perform (timeIn.data(), packed.data(), false);

                auto& specA = spectrum[(std::size_t) ch];
                for (int k = 0; k < bins; ++k)
                {
                    const Complex z  = packed[(std::size_t) k];
                    const Complex zc = std::conj (packed[(std::size_t) ((fftSize - k) & (fftSize - 1))]);
                    if (pair)
                    {
                        specA[(std::size_t) k] = 0.5f * (z + zc);
                        spectrum[(std::size_t) (ch + 1)][(std::size_t) k] = Complex { 0.0f, -0.5f } * (z - zc);
                    }
                    else
                    {
                        specA[(std::size_t) k] = z;
                    }
                }

                for (int c = ch; c < ch + (pair ? 2 : 1); ++c)
                    for (int k = 0; k < bins; ++k)
                        magnitude[(std::size_t) c][(std::size_t) k] = std::abs (spectrum[(std::size_t) c][(std::size_t) k]);
            }

            // ---- Shared phase rotation ---------------------------------------
            for (int k = 0; k < bins; ++k)
            {
                Complex mid { 0.0f, 0.0f };
                for (int ch = 0; ch < numChannels; ++ch)
                    mid += spectrum[(std::size_t) ch][(std::size_t) k];

                const Complex side = haveSide ? spectrum[0][(std::size_t) k] - spectrum[1][(std::size_t) k]
                                              : Complex { 0.0f, 0.0f };

                const float midPhase  = std::arg (mid);
                const float sidePhase = haveSide ? std::arg (side) : 0.0f;

                if (frame == 0 || frameHasOnset)
                {
                    // Start of the material, or an attack: take the analysis
                    // phase verbatim. Propagating through an onset is exactly
                    // what smears it.
                    rotation[(std::size_t) k] = 0.0f;
                }
                else
                {
                    const bool useSide = haveSide && std::abs (side) > std::abs (mid);
                    const float now  = useSide ? sidePhase : midPhase;
                    const float prev = useSide ? prevSide[(std::size_t) k] : prevMid[(std::size_t) k];

                    // Deviation from this bin's expected phase advance gives
                    // the partial's true frequency.
                    const float expected = omega[(std::size_t) k] * static_cast<float> (hopA);
                    const float delta    = principalArgument (now - prev - expected);
                    const float trueFreq = omega[(std::size_t) k] + delta / static_cast<float> (hopA);

                    rotation[(std::size_t) k] = principalArgument (
                        rotation[(std::size_t) k] + trueFreq * static_cast<float> (hopS - hopA));
                }

                prevMid[(std::size_t) k]  = midPhase;
                prevSide[(std::size_t) k] = sidePhase;
            }

            // ---- Formant warp ------------------------------------------------
            // Divide out the spectral envelope, resample the envelope along
            // the frequency axis, and multiply it back. The harmonics keep
            // their positions (pitch unchanged) while the resonances move.
            if (warpFormants)
            {
                for (int ch = 0; ch < numChannels; ch += 2)
                {
                    const bool pair = ch + 1 < numChannels;
                    spectralEnvelopes (magnitude[(std::size_t) ch].data(),
                                       pair ? magnitude[(std::size_t) (ch + 1)].data() : nullptr,
                                       bins, kLiftOrder,
                                       envelope.data() + (std::size_t) ch * (std::size_t) bins,
                                       pair ? envelope.data() + (std::size_t) (ch + 1) * (std::size_t) bins
                                            : nullptr,
                                       envWork);
                }

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto& mag = magnitude[(std::size_t) ch];
                    const float* env = envelope.data() + (std::size_t) ch * (std::size_t) bins;

                    for (int k = 0; k < bins; ++k)
                    {
                        // Where this bin's new envelope value comes from.
                        const double srcBin = static_cast<double> (k) / formantWarp;

                        float warped = 0.0f;
                        if (srcBin >= 0.0 && srcBin <= bins - 1)
                        {
                            const int   i0 = static_cast<int> (srcBin);
                            const int   i1 = juce::jmin (i0 + 1, bins - 1);
                            const float fr = static_cast<float> (srcBin - i0);
                            warped = env[i0] * (1.0f - fr) + env[i1] * fr;
                        }

                        // The complex bin is scaled along with its magnitude:
                        // resynthesis rotates the bin itself, not a rebuilt
                        // magnitude/phase pair.
                        const float original = env[k];
                        const float gain = (original > kEps) ? warped / original : 0.0f;
                        mag[(std::size_t) k] *= gain;
                        spectrum[(std::size_t) ch][(std::size_t) k] *= gain;
                    }
                }
            }

            // ---- Phase locking -----------------------------------------------
            // Rotate each spectral peak's neighbourhood rigidly by that peak's
            // rotation, instead of letting every bin drift on its own. This is
            // what keeps partials coherent and removes the watery "phasiness"
            // of a plain vocoder. Peaks are picked on the summed magnitude of
            // all channels, so every channel locks to the same regions.
            for (int k = 0; k < bins; ++k)
            {
                float w = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    w += magnitude[(std::size_t) ch][(std::size_t) k];
                peakWeight[(std::size_t) k] = w;
            }

            locked = rotation;

            peaks.clear();
            for (int k = 2; k < bins - 2; ++k)
            {
                const float m = peakWeight[(std::size_t) k];
                if (m > peakWeight[(std::size_t) (k - 1)] && m > peakWeight[(std::size_t) (k + 1)]
                    && m > peakWeight[(std::size_t) (k - 2)] && m > peakWeight[(std::size_t) (k + 2)])
                    peaks.push_back (k);
            }

            for (std::size_t p = 0; p < peaks.size(); ++p)
            {
                const int peak = peaks[p];

                // Region of influence: up to the midpoint between this peak
                // and its neighbours.
                const int lo = (p == 0) ? 0 : (peaks[p - 1] + peak) / 2 + 1;
                const int hi = (p + 1 == peaks.size()) ? bins - 1 : (peak + peaks[p + 1]) / 2;

                for (int k = lo; k <= hi; ++k)
                    locked[(std::size_t) k] = rotation[(std::size_t) peak];
            }

            // ---- Resynthesis -------------------------------------------------
            // Inverse of the analysis packing: build each channel's Hermitian
            // spectrum and send A + iB through one inverse transform, whose
            // real and imaginary parts are the two channels. DC and Nyquist
            // only ever contribute their real part to a real signal, so they
            // are made real here rather than leaking into the partner channel.
            for (int k = 0; k < bins; ++k)
                rotor[(std::size_t) k] = { std::cos (locked[(std::size_t) k]),
                                           std::sin (locked[(std::size_t) k]) };

            for (int ch = 0; ch < numChannels; ch += 2)
            {
                const bool pair = ch + 1 < numChannels;

                // Synthesis phase = analysis phase + the shared rotation, so
                // each output bin is just the analysis bin turned by that
                // rotation — one complex multiply, no per-channel trig.
                auto synth = [&] (int c, int k) noexcept
                {
                    return spectrum[(std::size_t) c][(std::size_t) k] * rotor[(std::size_t) k];
                };

                const Complex j { 0.0f, 1.0f };
                for (int k = 0; k < bins; ++k)
                {
                    Complex a = synth (ch, k);
                    Complex b = pair ? synth (ch + 1, k) : Complex { 0.0f, 0.0f };

                    if (k == 0 || k == fftSize / 2)
                    {
                        a = { a.real(), 0.0f };
                        b = { b.real(), 0.0f };
                    }

                    freqIn[(std::size_t) k] = a + j * b;
                    if (k > 0 && k < fftSize - k)
                        freqIn[(std::size_t) (fftSize - k)] = std::conj (a) + j * std::conj (b);
                }

                fft.perform (freqIn.data(), timeOut.data(), true);

                float* dstA = output.getWritePointer (ch);
                float* dstB = pair ? output.getWritePointer (ch + 1) : nullptr;
                for (int i = 0; i < fftSize; ++i)
                {
                    const int at = outPos + i;
                    if (at >= outLength)
                        break;

                    const float w = window[(std::size_t) i];
                    dstA[at] += timeOut[(std::size_t) i].real() * w;
                    if (dstB != nullptr)
                        dstB[at] += timeOut[(std::size_t) i].imag() * w;
                }
            }

            for (int i = 0; i < fftSize && outPos + i < outLength; ++i)
                windowSum[(std::size_t) (outPos + i)] += window[(std::size_t) i] * window[(std::size_t) i];
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
