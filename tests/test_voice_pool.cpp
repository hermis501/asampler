#include <catch2/catch_test_macros.hpp>

#include "engine/Sample.h"
#include "engine/Voice.h"
#include "engine/VoicePool.h"

#include <memory>

using namespace asampler;

namespace
{
    std::shared_ptr<const Sample> makeDC (int numSamples = 100)
    {
        juce::AudioBuffer<float> buf (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (0, i, 1.0f);
        return std::make_shared<const Sample> (std::move (buf), 44100.0);
    }

    VoiceConfig instantConfig()
    {
        VoiceConfig cfg;
        cfg.adsr = { 0.0f, 0.0f, 1.0f, 0.0f };
        return cfg;
    }

    VoiceConfig releaseConfig (float releaseSec)
    {
        VoiceConfig cfg;
        cfg.adsr = { 0.0f, 0.0f, 1.0f, releaseSec };
        return cfg;
    }
}

TEST_CASE ("VoicePool starts with zero active voices", "[pool]")
{
    VoicePool pool;
    pool.prepare (44100.0, 8);
    REQUIRE (pool.countActive() == 0);
}

TEST_CASE ("VoicePool counts one active voice after a trigger", "[pool]")
{
    VoicePool pool;
    pool.prepare (44100.0, 8);
    auto sample = makeDC();

    pool.triggerPad (0, sample, 60, instantConfig());
    REQUIRE (pool.countActive() == 1);
}

TEST_CASE ("VoicePool frees voices that exhaust their sample", "[pool]")
{
    VoicePool pool;
    pool.prepare (44100.0, 8);
    auto sample = makeDC (100);

    pool.triggerPad (0, sample, 60, instantConfig());

    juce::AudioBuffer<float> out (2, 200);
    out.clear();
    pool.renderAdding (out, 0, 200);

    REQUIRE (pool.countActive() == 0);
    REQUIRE (out.getSample (0, 0) > 0.5f);
}

TEST_CASE ("VoicePool releasePad only affects matching pads", "[pool]")
{
    VoicePool pool;
    pool.prepare (44100.0, 8);
    auto sample = makeDC (44100);

    pool.triggerPad (0, sample, 60, releaseConfig (0.005f));
    pool.triggerPad (1, sample, 61, releaseConfig (10.0f));
    REQUIRE (pool.countActive() == 2);

    pool.releasePad (0);

    juce::AudioBuffer<float> out (2, 500);
    out.clear();
    pool.renderAdding (out, 0, 500);

    REQUIRE (pool.countActive() == 1);
}

TEST_CASE ("VoicePool steals the oldest slot when full", "[pool]")
{
    VoicePool pool;
    pool.prepare (44100.0, 2);
    auto sample = makeDC (44100);

    pool.triggerPad (0, sample, 60, instantConfig());
    pool.triggerPad (1, sample, 61, instantConfig());
    pool.triggerPad (2, sample, 62, instantConfig());

    REQUIRE (pool.countActive() == 2);

    pool.releasePad (0);
    juce::AudioBuffer<float> out (2, 10);
    out.clear();
    pool.renderAdding (out, 0, 10);
    REQUIRE (pool.countActive() == 2);
}

TEST_CASE ("VoicePool chokePads hard-stops voices in the group immediately", "[pool][choke]")
{
    VoicePool pool;
    pool.prepare (44100.0, 8);
    auto sample = makeDC (44100);

    pool.triggerPad (0, sample, 60, instantConfig());
    pool.triggerPad (1, sample, 61, instantConfig());
    pool.triggerPad (2, sample, 62, instantConfig());
    REQUIRE (pool.countActive() == 3);

    const int chokeTargets[] = { 0, 2 };
    pool.chokePads (chokeTargets, 2);

    REQUIRE (pool.countActive() == 1);
}

TEST_CASE ("VoicePool refuses to trigger with a null sample", "[pool]")
{
    VoicePool pool;
    pool.prepare (44100.0, 4);

    pool.triggerPad (0, nullptr, 60, instantConfig());
    REQUIRE (pool.countActive() == 0);
}

TEST_CASE ("VoicePool per-pad render routes voices to their pad's destination",
           "[pool][routing]")
{
    VoicePool pool;
    pool.prepare (44100.0, 4);
    auto sample = makeDC (64);

    // Two voices on two pads: pad 0 and pad 1.
    pool.triggerPad (0, sample, 60, instantConfig());
    pool.triggerPad (1, sample, 60, instantConfig());

    juce::AudioBuffer<float> destPad0 (2, 64);
    juce::AudioBuffer<float> destPad1 (2, 64);
    destPad0.clear();
    destPad1.clear();

    // Build a routing table. Pads 2..15 won't be used in this test but must
    // still have a non-null entry (the caller-side guarantee).
    juce::AudioBuffer<float> sink (2, 64);
    sink.clear();
    std::array<juce::AudioBuffer<float>*, 16> destByPad {};
    destByPad[0] = &destPad0;
    destByPad[1] = &destPad1;
    for (int p = 2; p < 16; ++p)
        destByPad[(std::size_t) p] = &sink;

    pool.renderAdding (destByPad.data(), 0, 64);

    // Pad 0's voice should have written into destPad0; pad 1's into destPad1.
    auto bufferEnergy = [] (const juce::AudioBuffer<float>& buf)
    {
        float total = 0.0f;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                total += std::abs (buf.getSample (ch, i));
        return total;
    };

    REQUIRE (bufferEnergy (destPad0) > 0.0f);
    REQUIRE (bufferEnergy (destPad1) > 0.0f);
    REQUIRE (bufferEnergy (sink)     == 0.0f);
}
