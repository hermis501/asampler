#include "SampleAnalysis.h"
#include "Transient.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace asampler
{
    namespace
    {
        constexpr double kMaxAnalysisSeconds = 60.0;

        float toDb (double linear)
        {
            return linear > 1.0e-5 ? static_cast<float> (20.0 * std::log10 (linear)) : -100.0f;
        }

        // ---- Tempo -------------------------------------------------------
        //
        // Autocorrelate a log-energy onset envelope and take the strongest lag
        // in 60..200 BPM, weighted by a broad preference for ~110 BPM. The
        // weighting is what resolves the octave ambiguity every beat tracker
        // faces (a 140 BPM pulse also repeats at 70) towards the reading a
        // producer would give. A loop cut to a whole number of beats then
        // snaps to the tempo its length implies, which is exact.
        std::optional<double> estimateBpm (const std::vector<float>& mono, double sr)
        {
            const double seconds = static_cast<double> (mono.size()) / sr;
            if (seconds < 2.0)
                return std::nullopt;

            const int hop   = juce::jmax (1, juce::roundToInt (sr * 0.0058));   // ~5.8 ms
            const int frame = hop * 4;
            const int numFrames = (static_cast<int> (mono.size()) - frame) / hop;
            if (numFrames < 64)
                return std::nullopt;

            std::vector<double> novelty ((std::size_t) numFrames, 0.0);
            double prevLog = 0.0;
            for (int k = 0; k < numFrames; ++k)
            {
                double e = 0.0;
                const float* x = mono.data() + (std::size_t) k * (std::size_t) hop;
                for (int j = 0; j < frame; ++j)
                    e += static_cast<double> (x[j]) * x[j];

                const double logE = std::log (1.0e-9 + e);
                novelty[(std::size_t) k] = (k > 0) ? juce::jmax (0.0, logE - prevLog) : 0.0;
                prevLog = logE;
            }

            const double mean = std::accumulate (novelty.begin(), novelty.end(), 0.0) / numFrames;
            for (auto& v : novelty)
                v -= mean;

            auto autocorr = [&] (int lag)
            {
                double acc = 0.0;
                for (int k = 0; k + lag < numFrames; ++k)
                    acc += novelty[(std::size_t) k] * novelty[(std::size_t) (k + lag)];
                return acc / static_cast<double> (numFrames - lag);
            };

            const double r0 = autocorr (0);
            if (r0 <= 0.0)
                return std::nullopt;

            const double framesPerMinute = 60.0 * sr / hop;
            const int lagMin = juce::jmax (1, static_cast<int> (std::floor (framesPerMinute / 200.0)));
            const int lagMax = juce::jmin (numFrames / 2, static_cast<int> (std::ceil (framesPerMinute / 60.0)));
            if (lagMax <= lagMin + 2)
                return std::nullopt;

            std::vector<double> r ((std::size_t) (lagMax + 2), 0.0);
            for (int lag = lagMin - 1; lag <= lagMax + 1; ++lag)
                if (lag > 0 && lag < numFrames)
                    r[(std::size_t) lag] = autocorr (lag) / r0;

            int    bestLag   = -1;
            double bestScore = 0.0;
            for (int lag = lagMin; lag <= lagMax; ++lag)
            {
                const bool isPeak = r[(std::size_t) lag] >= r[(std::size_t) (lag - 1)]
                                 && r[(std::size_t) lag] >= r[(std::size_t) (lag + 1)];
                if (! isPeak)
                    continue;

                const double bpm   = framesPerMinute / lag;
                const double oct   = std::log2 (bpm / 110.0) / 0.9;
                const double score = r[(std::size_t) lag] * std::exp (-0.5 * oct * oct);
                if (score > bestScore) { bestScore = score; bestLag = lag; }
            }

            // Weak periodicity: no confident tempo, so say nothing.
            if (bestLag < 0 || r[(std::size_t) bestLag] < 0.2)
                return std::nullopt;

            // Parabolic refinement of the peak position.
            const double a = r[(std::size_t) (bestLag - 1)];
            const double b = r[(std::size_t) bestLag];
            const double c = r[(std::size_t) (bestLag + 1)];
            const double denom = a - 2.0 * b + c;
            const double shift = std::abs (denom) > 1.0e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denom) : 0.0;
            double bpm = framesPerMinute / (bestLag + shift);

            // Snap to the tempo implied by the length, if the loop is a whole
            // number of beats and that agrees with what was heard.
            for (int beats : { 4, 8, 12, 16, 24, 32, 48, 64 })
            {
                const double fromLength = 60.0 * beats / seconds;
                if (std::abs (fromLength / bpm - 1.0) < 0.03)
                {
                    bpm = fromLength;
                    break;
                }
            }

            return std::round (bpm * 10.0) / 10.0;
        }

        // ---- Pitch (YIN) -------------------------------------------------
        //
        // One estimate per frame across the material; reported only when most
        // sounding frames agree. A chord can look periodic in one frame, but
        // not consistently across a progression, so the agreement test is
        // what separates "a note" from "music".
        std::optional<float> yinFrame (const float* x, int window, int minTau, int maxTau, double sr)
        {
            std::vector<double> d ((std::size_t) maxTau + 2, 0.0);
            for (int tau = 1; tau <= maxTau + 1; ++tau)
            {
                double acc = 0.0;
                for (int j = 0; j < window; ++j)
                {
                    const double diff = static_cast<double> (x[j]) - x[j + tau];
                    acc += diff * diff;
                }
                d[(std::size_t) tau] = acc;
            }

            // Cumulative mean normalised difference.
            std::vector<double> cmnd ((std::size_t) maxTau + 2, 1.0);
            double running = 0.0;
            for (int tau = 1; tau <= maxTau + 1; ++tau)
            {
                running += d[(std::size_t) tau];
                cmnd[(std::size_t) tau] = running > 0.0 ? d[(std::size_t) tau] * tau / running : 1.0;
            }

            constexpr double threshold = 0.12;
            for (int tau = minTau; tau <= maxTau; ++tau)
            {
                if (cmnd[(std::size_t) tau] < threshold)
                {
                    while (tau + 1 <= maxTau && cmnd[(std::size_t) (tau + 1)] < cmnd[(std::size_t) tau])
                        ++tau;

                    const double a = cmnd[(std::size_t) (tau - 1)];
                    const double b = cmnd[(std::size_t) tau];
                    const double c = cmnd[(std::size_t) (tau + 1)];
                    const double denom = a - 2.0 * b + c;
                    const double shift = std::abs (denom) > 1.0e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denom) : 0.0;
                    const double f = sr / (tau + shift);
                    return static_cast<float> (69.0 + 12.0 * std::log2 (f / 440.0));
                }
            }
            return std::nullopt;
        }

        std::optional<float> estimatePitch (const std::vector<float>& mono, double sr)
        {
            const int maxTau = static_cast<int> (sr / 30.0);     // down to ~30 Hz (808 sub)
            const int minTau = juce::jmax (2, static_cast<int> (sr / 1500.0));
            const int window = juce::jmax (1024, maxTau);
            const int need   = window + maxTau + 2;
            const int skip   = static_cast<int> (sr * 0.03);    // past the attack click

            const int usable = static_cast<int> (mono.size()) - skip - need;
            if (usable < 0)
                return std::nullopt;

            constexpr int kFrames = 8;
            std::vector<float> estimates;
            int sounding = 0;

            for (int i = 0; i < kFrames; ++i)
            {
                const int pos = skip + (kFrames > 1 ? usable * i / (kFrames - 1) : 0);
                const float* x = mono.data() + pos;

                double energy = 0.0;
                for (int j = 0; j < window; ++j)
                    energy += static_cast<double> (x[j]) * x[j];
                if (toDb (std::sqrt (energy / window)) < -50.0f)
                    continue;   // tail silence says nothing about pitch

                ++sounding;
                if (auto est = yinFrame (x, window, minTau, maxTau, sr))
                    estimates.push_back (*est);
            }

            if (sounding == 0 || estimates.empty())
                return std::nullopt;

            auto sorted = estimates;
            std::sort (sorted.begin(), sorted.end());
            const float median = sorted[sorted.size() / 2];

            const auto agreeing = std::count_if (estimates.begin(), estimates.end(),
                                                 [median] (float m) { return std::abs (m - median) < 0.5f; });
            if (static_cast<double> (agreeing) < 0.75 * sounding)
                return std::nullopt;

            return median;
        }

        // ---- Key (chroma + Krumhansl-Kessler) ----------------------------

        std::optional<std::pair<int, bool>> estimateKey (const std::vector<float>& mono, double sr)
        {
            constexpr int order = 12;
            constexpr int size  = 1 << order;
            constexpr int hop   = size / 2;
            if ((int) mono.size() < size)
                return std::nullopt;

            juce::dsp::FFT fft (order);
            juce::dsp::WindowingFunction<float> window (size, juce::dsp::WindowingFunction<float>::hann, false);
            std::vector<float> buf ((std::size_t) size * 2, 0.0f);
            std::array<double, 12> chroma {};

            const double binHz = sr / size;
            const int lowBin   = juce::jmax (1, static_cast<int> (65.0 / binHz));
            const int highBin  = juce::jmin (size / 2 - 1, static_cast<int> (2100.0 / binHz));

            for (std::size_t pos = 0; pos + size <= mono.size(); pos += hop)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                std::copy (mono.begin() + (std::ptrdiff_t) pos, mono.begin() + (std::ptrdiff_t) (pos + size), buf.begin());
                window.multiplyWithWindowingTable (buf.data(), (size_t) size);
                fft.performFrequencyOnlyForwardTransform (buf.data());

                for (int bin = lowBin; bin <= highBin; ++bin)
                {
                    const double f    = bin * binHz;
                    const int    note = static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (f / 440.0)));
                    chroma[(std::size_t) (((note % 12) + 12) % 12)] += buf[(std::size_t) bin];
                }
            }

            const double total = std::accumulate (chroma.begin(), chroma.end(), 0.0);
            if (total <= 0.0)
                return std::nullopt;

            // Tonalness gate. Correlation is scale-invariant, so the tiny
            // ripples in a near-flat chroma (noise, cymbals) can still match a
            // key profile well. Real harmony concentrates energy in a few
            // pitch classes; require that spread before trusting a match.
            {
                const double mean = total / 12.0;
                double var = 0.0;
                for (double v : chroma) var += (v - mean) * (v - mean);
                if (std::sqrt (var / 12.0) / mean < 0.25)
                    return std::nullopt;
            }

            static constexpr std::array<double, 12> major { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
                                                            2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
            static constexpr std::array<double, 12> minor { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
                                                            2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

            auto pearson = [&chroma] (const std::array<double, 12>& profile, int tonic)
            {
                double mx = 0.0, my = 0.0;
                for (int i = 0; i < 12; ++i) { mx += chroma[(std::size_t) i]; my += profile[(std::size_t) i]; }
                mx /= 12.0; my /= 12.0;

                double sxy = 0.0, sxx = 0.0, syy = 0.0;
                for (int i = 0; i < 12; ++i)
                {
                    const double x = chroma[(std::size_t) ((i + tonic) % 12)] - mx;
                    const double y = profile[(std::size_t) i] - my;
                    sxy += x * y; sxx += x * x; syy += y * y;
                }
                return (sxx > 0.0 && syy > 0.0) ? sxy / std::sqrt (sxx * syy) : 0.0;
            };

            double best = -1.0, second = -1.0;
            int bestTonic = 0;
            bool bestMinor = false;
            for (int tonic = 0; tonic < 12; ++tonic)
                for (bool isMinor : { false, true })
                {
                    const double corr = pearson (isMinor ? minor : major, tonic);
                    if (corr > best)
                    {
                        second = best;
                        best = corr; bestTonic = tonic; bestMinor = isMinor;
                    }
                    else if (corr > second)
                    {
                        second = corr;
                    }
                }

            // Drums and noise correlate weakly with every key.
            if (best < 0.6)
                return std::nullopt;

            return std::make_pair (bestTonic, bestMinor);
        }
    }

    SampleAnalysis analyseSample (const juce::AudioBuffer<float>& audio, double sampleRate,
                                  int start, int end)
    {
        SampleAnalysis result;
        const int total = audio.getNumSamples();
        if (end < 0 || end > total) end = total;
        start = juce::jlimit (0, end, start);
        if (sampleRate <= 0.0 || end <= start || audio.getNumChannels() == 0)
            return result;

        result.lengthSeconds = (end - start) / sampleRate;

        const int count = juce::jmin (end - start, static_cast<int> (kMaxAnalysisSeconds * sampleRate));
        std::vector<float> mono ((std::size_t) count, 0.0f);
        const float scale = 1.0f / static_cast<float> (audio.getNumChannels());
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const float* src = audio.getReadPointer (ch, start);
            for (int i = 0; i < count; ++i)
                mono[(std::size_t) i] += src[i] * scale;
        }

        // Peak over every channel and the full window (not the mono mix, where
        // out-of-phase content would hide clipping).
        double peak = 0.0;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            peak = juce::jmax (peak, static_cast<double> (audio.getMagnitude (ch, start, end - start)));
        result.peakDb = toDb (peak);

        double sumSq = 0.0;
        for (float v : mono) sumSq += static_cast<double> (v) * v;
        result.rmsDb = toDb (std::sqrt (sumSq / juce::jmax (1, count)));

        if (result.peakDb <= -99.0f)
            return result;   // silence: nothing more to say

        {
            float* channels[] = { mono.data() };
            const juce::AudioBuffer<float> view (channels, 1, count);
            result.transientCount = static_cast<int> (detectTransients (view, sampleRate).size());
        }

        result.bpm       = estimateBpm (mono, sampleRate);
        result.pitchMidi = estimatePitch (mono, sampleRate);

        // A single sustained note has a pitch, not a key.
        if (! result.pitchMidi)
            if (auto key = estimateKey (mono, sampleRate))
            {
                result.keyTonic   = key->first;
                result.keyIsMinor = key->second;
            }

        return result;
    }

    juce::String keyName (int tonic, bool minor)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((tonic % 12) + 12) % 12]) + (minor ? " minor" : " major");
    }

    juce::String describePitch (float midiNote)
    {
        const int   nearest = juce::roundToInt (midiNote);
        const int   cents   = juce::roundToInt ((midiNote - static_cast<float> (nearest)) * 100.0f);
        const auto  name    = juce::MidiMessage::getMidiNoteName (nearest, true, true, 3);

        if (std::abs (cents) <= 5)
            return name + ", in tune";
        return name + ", " + juce::String (std::abs (cents)) + " cents " + (cents > 0 ? "sharp" : "flat");
    }
}
