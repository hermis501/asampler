#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/SampleAnalysis.h"

#include <cmath>

using namespace asampler;
using Catch::Approx;

namespace
{
    constexpr double kRate = 44100.0;
    constexpr float  kTwoPi = juce::MathConstants<float>::twoPi;

    float midiToHz (float m) { return 440.0f * std::pow (2.0f, (m - 69.0f) / 12.0f); }

    juce::AudioBuffer<float> sine (double seconds, float hz, float amp = 0.5f)
    {
        const int n = static_cast<int> (seconds * kRate);
        juce::AudioBuffer<float> b (1, n);
        for (int i = 0; i < n; ++i)
            b.setSample (0, i, amp * std::sin (kTwoPi * hz * (float) i / (float) kRate));
        return b;
    }

    // Short decaying noise bursts on every beat: a bare click track.
    juce::AudioBuffer<float> clickTrack (double bpm, double seconds)
    {
        const int n = static_cast<int> (seconds * kRate);
        juce::AudioBuffer<float> b (1, n);
        b.clear();
        juce::Random rng (42);
        const double beat = 60.0 / bpm * kRate;
        for (double t = 0.0; t < n; t += beat)
        {
            const int start = static_cast<int> (t);
            for (int i = 0; i < 2000 && start + i < n; ++i)
                b.setSample (0, start + i, (rng.nextFloat() * 2.0f - 1.0f)
                                           * std::exp (-(float) i / 300.0f) * 0.8f);
        }
        return b;
    }

    // One second per chord, three sine voices each with a quiet octave.
    juce::AudioBuffer<float> chords (std::initializer_list<std::initializer_list<int>> progression)
    {
        const int per = static_cast<int> (kRate);
        juce::AudioBuffer<float> b (1, per * (int) progression.size());
        b.clear();
        int c = 0;
        for (const auto& chord : progression)
        {
            for (int note : chord)
            {
                const float hz = midiToHz ((float) note);
                for (int i = 0; i < per; ++i)
                {
                    const float t = (float) i / (float) kRate;
                    b.addSample (0, c * per + i, 0.12f * std::sin (kTwoPi * hz * t)
                                               + 0.04f * std::sin (kTwoPi * 2.0f * hz * t));
                }
            }
            ++c;
        }
        return b;
    }
}

TEST_CASE ("Analysis reports length and peak level", "[analysis]")
{
    const auto a = analyseSample (sine (1.5, 440.0f, 0.5f), kRate);
    REQUIRE (a.lengthSeconds == Approx (1.5).margin (1.0e-3));
    REQUIRE (a.peakDb == Approx (-6.02f).margin (0.1f));
}

TEST_CASE ("Analysis respects the start/end window", "[analysis]")
{
    const auto a = analyseSample (sine (2.0, 440.0f), kRate, 22050, 66150);
    REQUIRE (a.lengthSeconds == Approx (1.0).margin (1.0e-3));
}

TEST_CASE ("A steady tone reports its pitch and no key", "[analysis][pitch]")
{
    const auto a = analyseSample (sine (1.0, 440.0f), kRate);
    REQUIRE (a.pitchMidi.has_value());
    REQUIRE (*a.pitchMidi == Approx (69.0f).margin (0.1f));
    REQUIRE_FALSE (a.keyTonic.has_value());
    REQUIRE_FALSE (a.bpm.has_value());   // too short to be a loop
}

TEST_CASE ("A decaying 808-style sub reports its low note", "[analysis][pitch]")
{
    const float hz = midiToHz (29.0f);   // F0 in the plugin's naming (middle C = C3), ~43.7 Hz
    auto b = sine (1.5, hz, 0.9f);
    for (int i = 0; i < b.getNumSamples(); ++i)
        b.setSample (0, i, b.getSample (0, i) * std::exp (-(float) i / (float) (kRate * 0.6)));

    const auto a = analyseSample (b, kRate);
    REQUIRE (a.pitchMidi.has_value());
    REQUIRE (*a.pitchMidi == Approx (29.0f).margin (0.3f));
}

TEST_CASE ("Click tracks report their tempo", "[analysis][bpm]")
{
    for (double bpm : { 90.0, 120.0, 140.0 })
    {
        CAPTURE (bpm);
        const auto a = analyseSample (clickTrack (bpm, 8.0), kRate);
        REQUIRE (a.bpm.has_value());
        REQUIRE (*a.bpm == Approx (bpm).margin (1.0));
        REQUIRE (a.transientCount >= 8);
    }
}

TEST_CASE ("A loop cut to whole bars snaps to its exact tempo", "[analysis][bpm]")
{
    // 4 bars at 93 BPM = 16 beats.
    const double seconds = 16.0 * 60.0 / 93.0;
    const auto a = analyseSample (clickTrack (93.0, seconds), kRate);
    REQUIRE (a.bpm.has_value());
    REQUIRE (*a.bpm == Approx (93.0).margin (0.05));
}

TEST_CASE ("A minor-key progression reports its key", "[analysis][key]")
{
    // Am - Dm - E - Am: the G# in E major marks it as A minor rather than
    // its relative, C major.
    const auto a = analyseSample (chords ({ { 57, 60, 64 }, { 62, 65, 69 },
                                            { 52, 56, 59 }, { 57, 60, 64 } }), kRate);
    REQUIRE_FALSE (a.pitchMidi.has_value());
    REQUIRE (a.keyTonic.has_value());
    REQUIRE (keyName (*a.keyTonic, a.keyIsMinor) == "A minor");
}

TEST_CASE ("Noise and silence make no musical claims", "[analysis]")
{
    juce::AudioBuffer<float> noise (1, static_cast<int> (3.0 * kRate));
    juce::Random rng (7);
    for (int i = 0; i < noise.getNumSamples(); ++i)
        noise.setSample (0, i, (rng.nextFloat() * 2.0f - 1.0f) * 0.3f);

    const auto n = analyseSample (noise, kRate);
    REQUIRE_FALSE (n.keyTonic.has_value());
    REQUIRE_FALSE (n.pitchMidi.has_value());

    juce::AudioBuffer<float> silence (2, 44100);
    silence.clear();
    const auto s = analyseSample (silence, kRate);
    REQUIRE (s.peakDb <= -99.0f);
    REQUIRE_FALSE (s.bpm.has_value());
    REQUIRE_FALSE (s.pitchMidi.has_value());
}

TEST_CASE ("Pitch descriptions name the note and the tuning error", "[analysis][pitch]")
{
    REQUIRE (describePitch (69.0f)  == "A3, in tune");
    REQUIRE (describePitch (29.12f) == "F0, 12 cents sharp");
    REQUIRE (describePitch (28.8f)  == "F0, 20 cents flat");
    REQUIRE (keyName (6, false) == "F# major");
}
