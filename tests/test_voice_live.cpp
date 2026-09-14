#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/Sample.h"
#include "engine/Voice.h"

#include <cmath>
#include <memory>

using namespace asampler;

namespace
{
    std::shared_ptr<const Sample> makeDC (int numSamples, double sourceRate = 44100.0)
    {
        juce::AudioBuffer<float> buf (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (0, i, 1.0f);
        return std::make_shared<const Sample> (std::move (buf), sourceRate);
    }

    // Sustain-forever envelope so the tests observe gain changes rather than
    // the ADSR decaying out from under them.
    VoiceConfig sustaining()
    {
        VoiceConfig cfg;
        cfg.adsr.attack  = 0.0f;
        cfg.adsr.decay   = 0.0f;
        cfg.adsr.sustain = 1.0f;
        cfg.adsr.release = 1.0f;
        return cfg;
    }

    float peakAbs (const juce::AudioBuffer<float>& buf, int channel, int start, int len)
    {
        float peak = 0.0f;
        for (int i = 0; i < len; ++i)
            peak = juce::jmax (peak, std::abs (buf.getSample (channel, start + i)));
        return peak;
    }
}

TEST_CASE ("Live gain push changes a sounding voice", "[voice][live]")
{
    Voice v;
    v.prepare (44100.0);

    auto cfg = sustaining();
    cfg.gainLin = 1.0f;
    v.start (makeDC (44100), 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 4096);
    out.clear();
    v.renderAdding (out, 0, 512);
    const float before = peakAbs (out, 0, 256, 128);

    // Halve the pad's volume while the note is still ringing.
    VoiceLiveParams live;
    live.gainLin = 0.5f;
    v.setLiveParams (live);

    out.clear();
    // Render well past the 20 ms smoothing ramp so the new value has settled.
    v.renderAdding (out, 0, 4096);
    const float after = peakAbs (out, 0, 3000, 512);

    REQUIRE (before > 0.5f);
    REQUIRE (after < before * 0.8f);
}

TEST_CASE ("Live gain changes are ramped, not stepped", "[voice][live][smoothing]")
{
    Voice v;
    v.prepare (44100.0);

    auto cfg = sustaining();
    cfg.gainLin = 1.0f;
    v.start (makeDC (44100), 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 64);
    out.clear();
    v.renderAdding (out, 0, 64);

    // Drop to silence. If the change were applied as a step, the very next
    // sample would already be at (or near) zero.
    VoiceLiveParams live;
    live.gainLin = 0.0f;
    v.setLiveParams (live);

    out.clear();
    v.renderAdding (out, 0, 8);

    // Still substantially audible a handful of samples in: 8 samples is far
    // less than the 20 ms (882 sample) ramp.
    REQUIRE (std::abs (out.getSample (0, 0)) > 0.5f);
}

TEST_CASE ("Live push preserves the hit's velocity scaling", "[voice][live][velocity]")
{
    Voice v;
    v.prepare (44100.0);

    // A quiet hit: pad volume 1.0 scaled by a velocity factor of 0.25.
    auto cfg = sustaining();
    cfg.gainLin      = 0.25f;
    cfg.hitGainScale = 0.25f;
    v.start (makeDC (44100), 60, 60, cfg);

    // Push the pad's *base* volume unchanged at 1.0. The voice must re-apply
    // its own 0.25 velocity scale rather than jumping to full level.
    VoiceLiveParams live;
    live.gainLin = 1.0f;
    v.setLiveParams (live);

    juce::AudioBuffer<float> out (2, 4096);
    out.clear();
    v.renderAdding (out, 0, 4096);

    const float settled = peakAbs (out, 0, 3000, 512);

    // Equal-power pan at centre is -3 dB, so a 0.25 scale lands near 0.177.
    REQUIRE (settled < 0.30f);
    REQUIRE (settled > 0.10f);
}

TEST_CASE ("Live push is ignored by an inactive voice", "[voice][live]")
{
    Voice v;
    v.prepare (44100.0);

    VoiceLiveParams live;
    live.gainLin = 0.5f;
    v.setLiveParams (live);   // must not crash or activate anything

    REQUIRE_FALSE (v.isActive());
}

TEST_CASE ("Live cutoff push reaches a sounding voice", "[voice][live][filter]")
{
    // A bright source through a low-pass whose cutoff is then dropped: the
    // output energy must fall.
    juce::AudioBuffer<float> buf (1, 44100);
    for (int i = 0; i < buf.getNumSamples(); ++i)
        buf.setSample (0, i, (i % 2 == 0) ? 1.0f : -1.0f);   // Nyquist-rate square
    auto source = std::make_shared<const Sample> (std::move (buf), 44100.0);

    Voice v;
    v.prepare (44100.0);

    auto cfg = sustaining();
    cfg.filterMode     = FilterMode::LowPass;
    cfg.filterCutoffHz = 20000.0f;
    v.start (source, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 8192);
    out.clear();
    v.renderAdding (out, 0, 4096);
    const float openPeak = peakAbs (out, 0, 2048, 1024);

    VoiceLiveParams live;
    live.filterCutoffHz = 200.0f;
    v.setLiveParams (live);

    out.clear();
    v.renderAdding (out, 0, 8192);
    const float closedPeak = peakAbs (out, 0, 6000, 1024);

    REQUIRE (closedPeak < openPeak * 0.5f);
}
