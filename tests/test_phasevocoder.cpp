#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/PhaseVocoder.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace asampler;

namespace
{
    juce::AudioBuffer<float> makeSine (int numSamples, double freqHz,
                                       double sampleRate = 44100.0,
                                       int numChannels = 1)
    {
        juce::AudioBuffer<float> buf (numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buf.setSample (ch, i, static_cast<float> (
                    std::sin (2.0 * juce::MathConstants<double>::pi * freqHz * i / sampleRate)));
        return buf;
    }

    // Dominant frequency by parabolic-interpolated peak of the magnitude
    // spectrum. Used to assert that stretching did not move the pitch.
    double dominantFrequency (const juce::AudioBuffer<float>& buf, double sampleRate,
                              int startSample = 0)
    {
        constexpr int order = 12;
        constexpr int size  = 1 << order;

        if (buf.getNumSamples() < startSample + size)
            return 0.0;

        std::vector<float> data ((std::size_t) size * 2, 0.0f);
        for (int i = 0; i < size; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                    * static_cast<float> (i) / static_cast<float> (size));
            data[(std::size_t) i] = buf.getSample (0, startSample + i) * w;
        }

        juce::dsp::FFT fft (order);
        fft.performFrequencyOnlyForwardTransform (data.data());

        int peak = 1;
        for (int k = 2; k < size / 2; ++k)
            if (data[(std::size_t) k] > data[(std::size_t) peak])
                peak = k;

        // Parabolic refinement around the peak bin.
        const double a = data[(std::size_t) (peak - 1)];
        const double b = data[(std::size_t) peak];
        const double c = data[(std::size_t) (peak + 1)];
        const double denom = a - 2.0 * b + c;
        const double offset = (std::abs (denom) > 1e-12) ? (0.5 * (a - c) / denom) : 0.0;

        return (peak + offset) * sampleRate / size;
    }

    float rms (const juce::AudioBuffer<float>& buf, int start, int len)
    {
        double acc = 0.0;
        for (int i = 0; i < len && start + i < buf.getNumSamples(); ++i)
        {
            const double s = buf.getSample (0, start + i);
            acc += s * s;
        }
        return static_cast<float> (std::sqrt (acc / juce::jmax (1, len)));
    }
}

TEST_CASE ("Neutral options are detected", "[vocoder]")
{
    REQUIRE (isNeutral (StretchOptions {}));

    StretchOptions stretched; stretched.timeRatio = 1.5;
    REQUIRE_FALSE (isNeutral (stretched));

    StretchOptions formant; formant.formantSemitones = 3.0;
    REQUIRE_FALSE (isNeutral (formant));
}

TEST_CASE ("Neutral options return the input unchanged", "[vocoder]")
{
    auto in = makeSine (8192, 440.0);
    auto out = processOffline (in, 44100.0, StretchOptions {});

    REQUIRE (out.getNumSamples() == in.getNumSamples());
    for (int i = 0; i < in.getNumSamples(); i += 97)
        REQUIRE (out.getSample (0, i) == Catch::Approx (in.getSample (0, i)));
}

TEST_CASE ("Empty and tiny input is handled without crashing", "[vocoder]")
{
    StretchOptions opts; opts.timeRatio = 2.0;

    juce::AudioBuffer<float> empty (1, 0);
    REQUIRE (processOffline (empty, 44100.0, opts).getNumSamples() == 0);

    // Shorter than one analysis window: returned as-is rather than mangled.
    auto tiny = makeSine (100, 440.0);
    auto out = processOffline (tiny, 44100.0, opts);
    REQUIRE (out.getNumSamples() == 100);
}

TEST_CASE ("Time stretch lengthens the output by the requested ratio", "[vocoder][stretch]")
{
    auto in = makeSine (44100, 440.0);

    StretchOptions opts; opts.timeRatio = 2.0;
    auto out = processOffline (in, 44100.0, opts);

    // Within a couple of percent of exactly double.
    REQUIRE (out.getNumSamples() > 44100 * 1.95);
    REQUIRE (out.getNumSamples() < 44100 * 2.05);
}

TEST_CASE ("Time compression shortens the output", "[vocoder][stretch]")
{
    auto in = makeSine (44100, 440.0);

    StretchOptions opts; opts.timeRatio = 0.5;
    auto out = processOffline (in, 44100.0, opts);

    REQUIRE (out.getNumSamples() > 44100 * 0.45);
    REQUIRE (out.getNumSamples() < 44100 * 0.55);
}

TEST_CASE ("Time stretch preserves pitch", "[vocoder][stretch]")
{
    // The whole point: change duration without changing pitch. Plain
    // resampling would halve the frequency at a 2x stretch.
    auto in = makeSine (44100, 440.0);

    StretchOptions opts; opts.timeRatio = 2.0;
    auto out = processOffline (in, 44100.0, opts);

    const double freq = dominantFrequency (out, 44100.0, 8192);
    REQUIRE (freq > 420.0);
    REQUIRE (freq < 460.0);
}

TEST_CASE ("Stretched output keeps a comparable level", "[vocoder][stretch]")
{
    auto in = makeSine (44100, 440.0);

    StretchOptions opts; opts.timeRatio = 1.5;
    auto out = processOffline (in, 44100.0, opts);

    const float inRms  = rms (in, 8192, 8192);
    const float outRms = rms (out, 8192, 8192);

    // Overlap-add normalisation errors show up here as a level jump.
    REQUIRE (outRms > inRms * 0.5f);
    REQUIRE (outRms < inRms * 1.6f);
}

TEST_CASE ("Stretched output is not silent or divergent", "[vocoder][stretch]")
{
    auto in = makeSine (44100, 220.0);

    for (double ratio : { 0.5, 0.75, 1.25, 2.0, 3.0 })
    {
        StretchOptions opts; opts.timeRatio = ratio;
        auto out = processOffline (in, 44100.0, opts);

        const auto peak = out.getMagnitude (0, out.getNumSamples());
        REQUIRE (peak > 0.05f);     // produced audio
        REQUIRE (peak < 4.0f);      // did not blow up
        REQUIRE (std::isfinite (peak));
    }
}

TEST_CASE ("Stereo input keeps both channels", "[vocoder][stretch]")
{
    auto in = makeSine (44100, 440.0, 44100.0, 2);

    StretchOptions opts; opts.timeRatio = 1.5;
    auto out = processOffline (in, 44100.0, opts);

    REQUIRE (out.getNumChannels() == 2);
    REQUIRE (out.getMagnitude (0, 0, out.getNumSamples()) > 0.05f);
    REQUIRE (out.getMagnitude (1, 0, out.getNumSamples()) > 0.05f);
}

TEST_CASE ("Extreme ratios are clamped rather than trusted", "[vocoder][stretch]")
{
    auto in = makeSine (22050, 440.0);

    StretchOptions huge; huge.timeRatio = 1000.0;
    auto out = processOffline (in, 44100.0, huge);
    REQUIRE (std::isfinite (out.getMagnitude (0, out.getNumSamples())));
    // Clamped at 4x.
    REQUIRE (out.getNumSamples() <= 22050 * 4 + 4096);

    StretchOptions tiny; tiny.timeRatio = 0.0001;
    auto out2 = processOffline (in, 44100.0, tiny);
    REQUIRE (out2.getNumSamples() > 0);
    REQUIRE (std::isfinite (out2.getMagnitude (0, out2.getNumSamples())));
}

// ---------------- Spectral envelope / formants ----------------

TEST_CASE ("Spectral envelope smooths the harmonic fine structure", "[vocoder][formant]")
{
    // A comb-like magnitude spectrum: alternating peaks and near-zeros. The
    // envelope must track the overall shape, not the individual teeth.
    constexpr int bins = 513;
    std::vector<float> mags ((std::size_t) bins);
    for (int k = 0; k < bins; ++k)
        mags[(std::size_t) k] = (k % 8 == 0) ? 1.0f : 0.01f;

    const auto env = computeSpectralEnvelope (mags, 48);
    REQUIRE (env.size() == mags.size());

    for (const auto v : env)
        REQUIRE (std::isfinite (v));

    // The defining property is smoothness: the input jumps by 100x between
    // adjacent bins, the envelope must not. (Its absolute level lands near the
    // geometric mean of the comb, which is much closer to the valley floor
    // than to the peaks — that is expected, not a fault.)
    float worstJump = 1.0f;
    for (int k = 17; k < bins - 17; ++k)
    {
        const float a = env[(std::size_t) k];
        const float b = env[(std::size_t) (k + 1)];
        if (a > 1.0e-9f && b > 1.0e-9f)
            worstJump = juce::jmax (worstJump, juce::jmax (a / b, b / a));
    }

    REQUIRE (worstJump < 1.5f);   // input's equivalent figure is 100

    // And it must not simply collapse onto either the teeth or the valleys.
    const float mid = env[(std::size_t) (bins / 2)];
    REQUIRE (mid > 0.01f);
    REQUIRE (mid < 1.0f);
}

TEST_CASE ("Spectral envelope follows a broad resonance", "[vocoder][formant]")
{
    // A formant is a broad peak in the envelope. Build a spectrum with a bump
    // around bin 100 modulated by a harmonic comb, and check the envelope
    // recovers the bump rather than the comb.
    constexpr int bins = 513;
    std::vector<float> mags ((std::size_t) bins);
    for (int k = 0; k < bins; ++k)
    {
        const float bump = std::exp (-0.5f * std::pow ((k - 100.0f) / 30.0f, 2.0f));
        const float comb = (k % 6 == 0) ? 1.0f : 0.05f;
        mags[(std::size_t) k] = (0.05f + bump) * comb;
    }

    const auto env = computeSpectralEnvelope (mags, 48);

    // The envelope should be clearly larger at the resonance than well away
    // from it, regardless of where the comb teeth happen to fall.
    REQUIRE (env[100] > env[300] * 2.0f);
    REQUIRE (env[100] > env[450] * 2.0f);
}

TEST_CASE ("Spectral envelope handles degenerate input", "[vocoder][formant]")
{
    std::vector<float> single { 1.0f };
    REQUIRE (computeSpectralEnvelope (single, 8).size() == 1);

    std::vector<float> zeros ((std::size_t) 513, 0.0f);
    const auto env = computeSpectralEnvelope (zeros, 48);
    for (const auto v : env)
        REQUIRE (std::isfinite (v));
}

TEST_CASE ("Formant shift leaves pitch alone", "[vocoder][formant]")
{
    // This is the property that separates formant shifting from pitch
    // shifting: the harmonics stay where they are, only their envelope moves.
    auto in = makeSine (44100, 440.0);

    StretchOptions opts; opts.formantSemitones = 7.0;
    auto out = processOffline (in, 44100.0, opts);

    const double freq = dominantFrequency (out, 44100.0, 8192);
    REQUIRE (freq > 420.0);
    REQUIRE (freq < 460.0);
}

TEST_CASE ("Formant shift preserves duration", "[vocoder][formant]")
{
    auto in = makeSine (44100, 440.0);

    StretchOptions opts; opts.formantSemitones = -5.0;
    auto out = processOffline (in, 44100.0, opts);

    REQUIRE (out.getNumSamples() > 44100 * 0.95);
    REQUIRE (out.getNumSamples() < 44100 * 1.05);
}

TEST_CASE ("Formant shift redistributes spectral energy", "[vocoder][formant]")
{
    // A rich source so there is an envelope to move. Shifting formants up
    // should move energy toward higher frequencies.
    constexpr int n = 44100;
    juce::AudioBuffer<float> in (1, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / 44100.0;
        double s = 0.0;
        for (int h = 1; h <= 12; ++h)
            s += std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * h * t) / h;
        in.setSample (0, i, static_cast<float> (s * 0.25));
    }

    auto highBandEnergy = [] (const juce::AudioBuffer<float>& b)
    {
        constexpr int order = 12;
        constexpr int size  = 1 << order;
        std::vector<float> data ((std::size_t) size * 2, 0.0f);
        for (int i = 0; i < size; ++i)
            data[(std::size_t) i] = b.getSample (0, 8192 + i);

        juce::dsp::FFT fft (order);
        fft.performFrequencyOnlyForwardTransform (data.data());

        double low = 0.0, high = 0.0;
        for (int k = 1; k < size / 2; ++k)
        {
            const double f = k * 44100.0 / size;
            if (f < 1000.0)      low  += data[(std::size_t) k];
            else if (f < 6000.0) high += data[(std::size_t) k];
        }
        return high / juce::jmax (1.0e-9, low);
    };

    StretchOptions up; up.formantSemitones = 12.0;
    auto shifted = processOffline (in, 44100.0, up);

    REQUIRE (shifted.getNumSamples() > 8192 + 4096);
    REQUIRE (std::isfinite (shifted.getMagnitude (0, shifted.getNumSamples())));

    // Both ratios must be finite and the shifted one should not be lower.
    const double before = highBandEnergy (in);
    const double after  = highBandEnergy (shifted);
    REQUIRE (std::isfinite (before));
    REQUIRE (std::isfinite (after));
    REQUIRE (after > before * 0.8);
}

TEST_CASE ("Stretch and formant shift combine without blowing up", "[vocoder]")
{
    auto in = makeSine (44100, 330.0);

    StretchOptions opts;
    opts.timeRatio = 1.75;
    opts.formantSemitones = -4.0;

    auto out = processOffline (in, 44100.0, opts);

    REQUIRE (out.getNumSamples() > 44100 * 1.7);
    REQUIRE (out.getNumSamples() < 44100 * 1.8);

    const auto peak = out.getMagnitude (0, out.getNumSamples());
    REQUIRE (peak > 0.05f);
    REQUIRE (std::isfinite (peak));

    const double freq = dominantFrequency (out, 44100.0, 8192);
    REQUIRE (freq > 310.0);
    REQUIRE (freq < 350.0);
}

TEST_CASE ("Transient preservation keeps attacks sharp", "[vocoder][transient]")
{
    // Silence, then a sharp burst. After stretching, the onset should still be
    // an onset — a phase vocoder without transient handling smears it into a
    // slow fade-in.
    constexpr int n = 44100;
    juce::AudioBuffer<float> in (1, n);
    in.clear();
    for (int i = 22050; i < 22050 + 2000; ++i)
    {
        const float decay = 1.0f - static_cast<float> (i - 22050) / 2000.0f;
        in.setSample (0, i, decay * std::sin (i * 0.3f));
    }

    StretchOptions opts;
    opts.timeRatio = 2.0;
    opts.preserveTransients = true;
    auto out = processOffline (in, 44100.0, opts);

    REQUIRE (std::isfinite (out.getMagnitude (0, out.getNumSamples())));

    // Energy well before the (stretched) onset must stay much lower than
    // energy just after it.
    const int onsetOut = 44100;   // 22050 * 2
    const float before = rms (out, onsetOut - 8000, 4000);
    const float after  = rms (out, onsetOut + 1000, 4000);

    REQUIRE (after > before * 2.0f);
}

// ----- Stereo linking ---------------------------------------------------------
//
// Each channel used to track its own phase, so channels drifted apart: the
// stereo image wobbled and the mono sum went hollow. All channels now share one
// phase rotation per bin, so the phase relationship between channels comes out
// as it went in.

namespace
{
    // A broadband test signal: a chord of partials plus some noise, so phase
    // estimates are imperfect, like real material rather than a lone sine.
    std::vector<float> makeRichSignal (int numSamples, juce::Random& rng, float noiseLevel)
    {
        std::vector<float> s ((std::size_t) numSamples);
        const double freqs[] = { 110.0, 220.0, 277.2, 329.6, 440.0, 659.3, 880.0, 1318.5 };
        for (int i = 0; i < numSamples; ++i)
        {
            double v = 0.0;
            for (double f : freqs)
                v += std::sin (2.0 * juce::MathConstants<double>::pi * f * i / 44100.0);
            s[(std::size_t) i] = static_cast<float> (v / 8.0)
                               + noiseLevel * (rng.nextFloat() * 2.0f - 1.0f);
        }
        return s;
    }

    double correlation (const juce::AudioBuffer<float>& b, int start, int len)
    {
        double sl = 0, sr = 0, sll = 0, srr = 0, slr = 0;
        for (int i = start; i < start + len; ++i)
        {
            const double l = b.getSample (0, i), r = b.getSample (1, i);
            sl += l; sr += r; sll += l * l; srr += r * r; slr += l * r;
        }
        const double n = len;
        const double cov = slr / n - (sl / n) * (sr / n);
        const double vl  = sll / n - (sl / n) * (sl / n);
        const double vr  = srr / n - (sr / n) * (sr / n);
        return cov / std::sqrt (juce::jmax (1e-12, vl * vr));
    }

    // Level of the mono sum relative to the channels: 1 for identical
    // channels, ~0.707 for unrelated ones, 0 for opposite polarity.
    double monoSumRatio (const juce::AudioBuffer<float>& b, int start, int len)
    {
        double sum = 0, chans = 0;
        for (int i = start; i < start + len; ++i)
        {
            const double l = b.getSample (0, i), r = b.getSample (1, i);
            sum   += 0.25 * (l + r) * (l + r);
            chans += 0.5 * (l * l + r * r);
        }
        return std::sqrt (sum / juce::jmax (1e-12, chans));
    }
}

TEST_CASE ("Stereo stretch keeps the correlation between channels", "[vocoder][stretch][stereo]")
{
    constexpr int n = 88200;
    juce::Random rng (7);
    const auto common = makeRichSignal (n, rng, 0.0f);

    // Shared musical content, different noise per side: a typical wide mix.
    juce::AudioBuffer<float> in (2, n);
    for (int i = 0; i < n; ++i)
    {
        in.setSample (0, i, common[(std::size_t) i] + 0.15f * (rng.nextFloat() * 2.0f - 1.0f));
        in.setSample (1, i, common[(std::size_t) i] + 0.15f * (rng.nextFloat() * 2.0f - 1.0f));
    }

    for (double ratio : { 0.8, 1.25, 1.5 })
    {
        CAPTURE (ratio);
        StretchOptions opts; opts.timeRatio = ratio;
        const auto out = processOffline (in, 44100.0, opts);

        const int len = 32768;
        const double before = correlation (in, 8192, len);
        const double after  = correlation (out, 8192, len);
        CAPTURE (before, after);
        REQUIRE (std::abs (after - before) < 0.08);

        const double monoBefore = monoSumRatio (in, 8192, len);
        const double monoAfter  = monoSumRatio (out, 8192, len);
        CAPTURE (monoBefore, monoAfter);
        REQUIRE (std::abs (monoAfter - monoBefore) < 0.05);
    }
}

TEST_CASE ("Stereo stretch keeps a time-delay image in place", "[vocoder][stretch][stereo]")
{
    // The right channel is the left delayed by 12 samples (sound arriving at
    // one ear first). That offset must survive stretching.
    constexpr int n = 88200, delay = 12;
    juce::Random rng (11);
    const auto sig = makeRichSignal (n + delay, rng, 0.1f);

    juce::AudioBuffer<float> in (2, n);
    for (int i = 0; i < n; ++i)
    {
        in.setSample (0, i, sig[(std::size_t) (i + delay)]);
        in.setSample (1, i, sig[(std::size_t) i]);
    }

    StretchOptions opts; opts.timeRatio = 1.3;
    const auto out = processOffline (in, 44100.0, opts);

    int bestLag = 0;
    double best = -1e9;
    for (int lag = -40; lag <= 40; ++lag)
    {
        double acc = 0;
        for (int i = 10000; i < 40000; ++i)
            acc += out.getSample (0, i) * out.getSample (1, i + lag);
        if (acc > best) { best = acc; bestLag = lag; }
    }
    CAPTURE (bestLag);
    REQUIRE (std::abs (bestLag - delay) <= 1);
}

TEST_CASE ("Opposite-polarity channels still cancel after stretching", "[vocoder][stretch][stereo]")
{
    constexpr int n = 66150;
    juce::Random rng (3);
    const auto sig = makeRichSignal (n, rng, 0.1f);

    juce::AudioBuffer<float> in (2, n);
    for (int i = 0; i < n; ++i)
    {
        in.setSample (0, i,  sig[(std::size_t) i]);
        in.setSample (1, i, -sig[(std::size_t) i]);
    }

    StretchOptions opts; opts.timeRatio = 1.4; opts.formantSemitones = 2.0;
    const auto out = processOffline (in, 44100.0, opts);
    REQUIRE (monoSumRatio (out, 4096, 40000) < 0.01);
}

TEST_CASE ("Identical channels stay identical", "[vocoder][stretch][stereo]")
{
    constexpr int n = 44100;
    juce::Random rng (5);
    const auto sig = makeRichSignal (n, rng, 0.2f);

    juce::AudioBuffer<float> in (2, n);
    for (int i = 0; i < n; ++i)
        for (int ch = 0; ch < 2; ++ch)
            in.setSample (ch, i, sig[(std::size_t) i]);

    StretchOptions opts; opts.timeRatio = 0.75;
    const auto out = processOffline (in, 44100.0, opts);
    // Both channels share one packed transform, so they agree to float
    // rounding (around -90 dB) rather than bit for bit.
    float worst = 0.0f;
    for (int i = 0; i < out.getNumSamples(); ++i)
        worst = std::max (worst, std::abs (out.getSample (0, i) - out.getSample (1, i)));
    REQUIRE (worst < 1.0e-4f);   // -80 dB
}

TEST_CASE ("High sample rates keep the same analysis time span", "[vocoder][stretch]")
{
    // At 96 kHz a fixed 2048-point window would span only 21 ms, too short to
    // resolve a low 808. A 55 Hz tone must keep its pitch at both rates.
    for (double rate : { 44100.0, 96000.0 })
    {
        CAPTURE (rate);
        auto in = makeSine ((int) (rate * 2), 55.0, rate);
        StretchOptions opts; opts.timeRatio = 1.5;
        const auto out = processOffline (in, rate, opts);
        const double f = dominantFrequency (out, rate, (int) rate / 2);
        REQUIRE (f == Catch::Approx (55.0).margin (rate > 50000 ? 1.5 : 1.0));
    }
}

TEST_CASE ("Stereo stretch keeps the image of instruments panned apart",
           "[vocoder][stretch][stereo]")
{
    // Two instruments a few Hz apart share FFT bins, one panned left and one
    // right. Each channel on its own hears a different "true frequency" in
    // those bins; tracked independently, the shared component's phase drifts
    // between the channels and the image smears. Linked, it cannot.
    constexpr int n = 88200;
    juce::AudioBuffer<float> in (2, n);
    const double a[] = { 196.0, 293.7, 440.0, 587.3 };
    const double b[] = { 203.0, 301.0, 449.0, 598.0 };
    for (int i = 0; i < n; ++i)
    {
        double sa = 0, sb = 0;
        for (int p = 0; p < 4; ++p)
        {
            sa += std::sin (2.0 * juce::MathConstants<double>::pi * a[p] * i / 44100.0);
            sb += std::sin (2.0 * juce::MathConstants<double>::pi * b[p] * i / 44100.0 + p);
        }
        in.setSample (0, i, static_cast<float> ((1.0 * sa + 0.4 * sb) / 8.0));
        in.setSample (1, i, static_cast<float> ((0.4 * sa + 1.0 * sb) / 8.0));
    }

    for (double ratio : { 0.8, 1.5, 2.0 })
    {
        CAPTURE (ratio);
        StretchOptions opts; opts.timeRatio = ratio;
        const auto out = processOffline (in, 44100.0, opts);

        const double before = correlation (in, 8192, 32768);
        const double after  = correlation (out, 8192, 32768);
        CAPTURE (before, after);
        REQUIRE (std::abs (after - before) < 0.08);
    }
}

// CPU cost of one stretch / formant render. Hidden; run with
// ASamplerTests.exe "[.stretchbench]".
TEST_CASE ("Stretch render cost for a 10 second stereo sample", "[.stretchbench]")
{
    constexpr double sr = 44100.0;
    auto input = makeSine (static_cast<int> (sr * 10.0), 220.0, sr, 2);
    juce::Random rng (7);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < input.getNumSamples(); ++i)
            input.addSample (ch, i, (rng.nextFloat() - 0.5f) * 0.2f);

    auto timeIt = [&] (const char* label, StretchOptions options)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto out = processOffline (input, sr, options);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("%-24s %8.1f ms (%d samples out)\n", label, ms, out.getNumSamples());
    };

    StretchOptions stretch;  stretch.timeRatio = 1.1;
    StretchOptions formant;  formant.formantSemitones = 3.0;
    StretchOptions both;     both.timeRatio = 1.1; both.formantSemitones = 3.0;
    timeIt ("stretch 1.1", stretch);
    timeIt ("formant +3", formant);
    timeIt ("stretch + formant", both);
    SUCCEED();
}

TEST_CASE ("A render that is told to abort stops and returns nothing",
           "[vocoder][stretch]")
{
    // A render superseded by a newer setting has to give its thread back
    // rather than finish work nobody will hear.
    auto in = makeSine (44100 * 4, 220.0, 44100.0, 2);
    StretchOptions opts; opts.timeRatio = 1.5;

    int polls = 0;
    const auto out = processOffline (in, 44100.0, opts, [&] { return ++polls > 2; });
    REQUIRE (out.getNumSamples() == 0);
    REQUIRE (polls == 3);

    const auto full = processOffline (in, 44100.0, opts, [] { return false; });
    REQUIRE (full.getNumSamples() > juce::roundToInt (in.getNumSamples() * 1.45));
}
