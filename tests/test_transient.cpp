#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/Transient.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

using namespace asampler;

namespace
{
    constexpr double kSr = 44100.0;

    // Writes a short decaying sine burst into `buf` starting at `startSample`.
    // Bursts are the smallest signal that resembles a percussive transient:
    // immediate energy onset, fast decay.
    void writeBurst (juce::AudioBuffer<float>& buf, int startSample,
                     int durationSamples = 2205, float freqHz = 440.0f,
                     float peak = 0.8f, float decayRate = 30.0f)
    {
        const int numChannels = buf.getNumChannels();
        const int totalSamples = buf.getNumSamples();
        for (int n = 0; n < durationSamples && (startSample + n) < totalSamples; ++n)
        {
            const float t   = static_cast<float> (n) / static_cast<float> (kSr);
            const float env = std::exp (-decayRate * t);
            const float s   = std::sin (juce::MathConstants<float>::twoPi * freqHz * t)
                            * env * peak;
            for (int ch = 0; ch < numChannels; ++ch)
                buf.addSample (ch, startSample + n, s);
        }
    }

    juce::AudioBuffer<float> makeSilent (int numSamples, int numChannels = 1)
    {
        juce::AudioBuffer<float> buf (numChannels, numSamples);
        buf.clear();
        return buf;
    }
}

TEST_CASE ("detectTransients returns empty for an empty buffer", "[transient]")
{
    juce::AudioBuffer<float> empty (1, 0);
    REQUIRE (detectTransients (empty, kSr).empty());
}

TEST_CASE ("detectTransients returns empty for buffers shorter than the window", "[transient]")
{
    auto tiny = makeSilent (256);  // shorter than the 1024-sample window
    REQUIRE (detectTransients (tiny, kSr).empty());
}

TEST_CASE ("detectTransients returns empty for pure silence", "[transient]")
{
    auto silent = makeSilent (static_cast<int> (kSr));
    REQUIRE (detectTransients (silent, kSr).empty());
}

TEST_CASE ("detectTransients finds bursts at their start positions",
           "[transient]")
{
    // Five bursts at known positions. Detection should land near each one
    // (within the frame resolution of the algorithm).
    auto buf = makeSilent (static_cast<int> (kSr * 1.0));  // 1 second mono
    const int positions[] = { 4410, 13230, 22050, 30870, 39690 };
    for (int p : positions) writeBurst (buf, p);

    const auto hits = detectTransients (buf, kSr);

    REQUIRE (hits.size() >= 5);

    // Hits must be sorted ascending by sampleIndex.
    for (std::size_t i = 1; i < hits.size(); ++i)
        REQUIRE (hits[i].sampleIndex >= hits[i - 1].sampleIndex);

    // Each expected position should have at least one hit within 20 ms
    // (~882 samples — comfortably wider than the 256-sample hop).
    for (int expected : positions)
    {
        bool found = false;
        for (const auto& h : hits)
            if (std::abs (h.sampleIndex - expected) < 882)
            {
                found = true;
                break;
            }
        CAPTURE (expected);
        REQUIRE (found);
    }
}

TEST_CASE ("detectTransients reports stronger hits with higher strength",
           "[transient]")
{
    // Two bursts of different peak amplitude. The louder one should report
    // a higher strength value.
    auto buf = makeSilent (static_cast<int> (kSr * 0.6));
    writeBurst (buf,  4410, 2205, 440.0f, 0.2f);   // quiet
    writeBurst (buf, 17640, 2205, 440.0f, 0.9f);   // loud

    const auto hits = detectTransients (buf, kSr);
    REQUIRE (hits.size() >= 2);

    // Find the hits nearest each burst.
    auto nearest = [&] (int targetSample) -> TransientHit
    {
        TransientHit best { -1, 0.0f };
        int bestDist = std::numeric_limits<int>::max();
        for (const auto& h : hits)
        {
            const int d = std::abs (h.sampleIndex - targetSample);
            if (d < bestDist) { best = h; bestDist = d; }
        }
        return best;
    };

    const auto quiet = nearest (4410);
    const auto loud  = nearest (17640);

    REQUIRE (quiet.sampleIndex >= 0);
    REQUIRE (loud.sampleIndex  >= 0);
    REQUIRE (loud.strength > quiet.strength);
}

TEST_CASE ("detectTransients enforces a minimum gap between hits", "[transient]")
{
    // Two bursts only 10 ms apart — well inside the 40 ms minimum-gap rule.
    // The two onsets must collapse into a single reported hit. With only one
    // event in the buffer, the property to verify is simply "no more than one
    // hit reported" — independent of where exactly the algorithm localises it.
    auto buf = makeSilent (static_cast<int> (kSr * 0.5));
    writeBurst (buf,  4410);
    writeBurst (buf,  4410 + 441);  // 10 ms later

    const auto hits = detectTransients (buf, kSr);

    REQUIRE (hits.size() == 1);
}

// ----- Spectral-flux variant -----------------------------------------------

TEST_CASE ("detectTransients (SpectralFlux) returns empty for empty buffer",
           "[transient][spectral]")
{
    juce::AudioBuffer<float> empty (1, 0);
    REQUIRE (detectTransients (empty, kSr, OnsetMethod::SpectralFlux).empty());
}

TEST_CASE ("detectTransients (SpectralFlux) returns empty for buffers shorter than its FFT window",
           "[transient][spectral]")
{
    // Spectral flux uses a 1024-sample FFT; anything shorter has no frames.
    auto tiny = makeSilent (512);
    REQUIRE (detectTransients (tiny, kSr, OnsetMethod::SpectralFlux).empty());
}

TEST_CASE ("detectTransients (SpectralFlux) returns empty for pure silence",
           "[transient][spectral]")
{
    auto silent = makeSilent (static_cast<int> (kSr));
    REQUIRE (detectTransients (silent, kSr, OnsetMethod::SpectralFlux).empty());
}

TEST_CASE ("detectTransients (SpectralFlux) finds bursts at their start positions",
           "[transient][spectral]")
{
    // Same fixture as the log-energy test: 5 known burst positions. Spectral
    // flux should pick them up too (a burst is a strong wideband event).
    auto buf = makeSilent (static_cast<int> (kSr * 1.0));
    const int positions[] = { 4410, 13230, 22050, 30870, 39690 };
    for (int p : positions) writeBurst (buf, p);

    const auto hits = detectTransients (buf, kSr, OnsetMethod::SpectralFlux);

    REQUIRE (hits.size() >= 5);

    for (std::size_t i = 1; i < hits.size(); ++i)
        REQUIRE (hits[i].sampleIndex >= hits[i - 1].sampleIndex);

    // Spectral flux's 1024-sample FFT means hits can land up to ~23 ms past
    // the actual attack. Allow ~30 ms tolerance to absorb that.
    for (int expected : positions)
    {
        bool found = false;
        for (const auto& h : hits)
            if (std::abs (h.sampleIndex - expected) < 1500)
            {
                found = true;
                break;
            }
        CAPTURE (expected);
        REQUIRE (found);
    }
}

TEST_CASE ("detectTransients (SpectralFlux) detects timbral changes",
           "[transient][spectral]")
{
    // Steady-amplitude tone that switches frequency at the midpoint. Energy
    // stays roughly flat on either side; the switch shows up as a strong
    // shift in the magnitude spectrum, which is exactly what flux measures.
    // Phase-aligned splice (both sines are at phase 0 at t = 0.3s) keeps the
    // amplitude continuous; only the timbre changes.
    const int    totalSamples = static_cast<int> (kSr * 0.6);
    const int    switchSample = totalSamples / 2;
    const double sr = kSr;
    juce::AudioBuffer<float> buf (1, totalSamples);
    auto* w = buf.getWritePointer (0);
    for (int i = 0; i < totalSamples; ++i)
    {
        const double t    = static_cast<double> (i) / sr;
        const double freq = (i < switchSample) ? 220.0 : 880.0;
        w[i] = static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * freq * t)
                                     * 0.5);
    }

    const auto fluxHits = detectTransients (buf, kSr, OnsetMethod::SpectralFlux);

    REQUIRE_FALSE (fluxHits.empty());

    // At least one flux hit must sit close to the transition. ~46 ms window
    // matches the FFT window straddling the switch.
    bool nearSwitch = false;
    for (const auto& h : fluxHits)
        if (std::abs (h.sampleIndex - switchSample) < 2048)
        {
            nearSwitch = true;
            break;
        }
    REQUIRE (nearSwitch);
}
