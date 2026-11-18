#include <catch2/catch_test_macros.hpp>

#include "engine/Sample.h"
#include "engine/Voice.h"
#include "engine/VoicePool.h"

#include <cmath>
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

TEST_CASE ("VoicePool chokePads stops voices in the group at once (they only fade out)", "[pool][choke]")
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
    REQUIRE_FALSE (pool.hasActivePad (0));
    REQUIRE (pool.countFading() == 2);

    // The fade is over within a few milliseconds.
    juce::AudioBuffer<float> out (2, 512);
    out.clear();
    pool.renderAdding (out, 0, 512);
    REQUIRE (pool.countFading() == 0);
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

namespace
{
    std::shared_ptr<const Sample> makeSilence (int numSamples)
    {
        juce::AudioBuffer<float> buf (1, numSamples);
        buf.clear();
        return std::make_shared<const Sample> (std::move (buf), 44100.0);
    }

    // Largest step between neighbouring output samples.
    float largestStep (const juce::AudioBuffer<float>& b, int from, int to)
    {
        float worst = 0.0f;
        for (int i = from + 1; i < to; ++i)
            worst = juce::jmax (worst, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));
        return worst;
    }
}

TEST_CASE ("A choked voice fades out instead of clicking", "[pool][choke][declick]")
{
    VoicePool pool;
    pool.prepare (44100.0, 8);
    pool.triggerPad (0, makeDC (44100), 60, instantConfig());

    juce::AudioBuffer<float> out (2, 400);
    out.clear();
    pool.renderAdding (out, 0, 100);
    const int choke[] = { 0 };
    pool.chokePads (choke, 1);
    pool.renderAdding (out, 100, 300);

    const int fade = (int) std::lround (VoicePool::kStopFadeSeconds * 44100.0);
    // Full level up to the choke, then a ramp: no step bigger than one
    // ramp increment, and silence once it is done.
    REQUIRE (out.getSample (0, 99) > 0.5f);
    REQUIRE (out.getSample (0, 100 + fade / 2) > 0.1f);
    REQUIRE (largestStep (out, 90, 400) < 2.0f / (float) fade);
    REQUIRE (out.getSample (0, 100 + fade + 2) == 0.0f);
    REQUIRE (pool.countFading() == 0);
}

TEST_CASE ("A mono pad's retrigger fades the old note out", "[pool][declick]")
{
    VoicePool pool;
    pool.prepare (44100.0, 8);
    pool.triggerPad (0, makeDC (44100), 60, instantConfig(), 1);

    juce::AudioBuffer<float> out (2, 400);
    out.clear();
    pool.renderAdding (out, 0, 100);

    // The new note is silent, so the output is the old note alone.
    pool.triggerPad (0, makeSilence (44100), 61, instantConfig(), 1);
    REQUIRE (pool.countActive() == 1);    // still one note: the cap holds
    REQUIRE (pool.countFading() == 1);
    pool.renderAdding (out, 100, 300);

    const int fade = (int) std::lround (VoicePool::kStopFadeSeconds * 44100.0);
    REQUIRE (largestStep (out, 90, 400) < 2.0f / (float) fade);
    REQUIRE (out.getSample (0, 100 + fade + 2) == 0.0f);
    REQUIRE (pool.countFading() == 0);
    REQUIRE (pool.countActive() == 1);
}

TEST_CASE ("A full pool reuses a fading voice before a sounding one", "[pool][declick]")
{
    VoicePool pool;
    pool.prepare (44100.0, 2);
    auto sample = makeDC (44100);
    pool.triggerPad (0, sample, 60, instantConfig());
    pool.triggerPad (1, sample, 61, instantConfig());

    const int choke[] = { 0 };
    pool.chokePads (choke, 1);            // pad 0 fading, pad 1 sounding
    pool.triggerPad (2, sample, 62, instantConfig());

    REQUIRE (pool.hasActivePad (1));      // the older-but-fading pad 0 gave way
    REQUIRE (pool.hasActivePad (2));
    REQUIRE (pool.countActive() == 2);
    REQUIRE (pool.countFading() == 0);
}

TEST_CASE ("A fading voice is not released, choked or stolen again", "[pool][declick]")
{
    VoicePool pool;
    pool.prepare (44100.0, 8);
    auto sample = makeDC (44100);
    pool.triggerPad (0, sample, 60, releaseConfig (2.0f), 1);
    pool.triggerPad (0, sample, 60, releaseConfig (2.0f), 1);   // first one fades
    REQUIRE (pool.countFading() == 1);

    // Another retrigger steals the newer note, not the fading one, and the
    // pad still holds exactly one note.
    pool.triggerPad (0, sample, 60, releaseConfig (2.0f), 1);
    REQUIRE (pool.countActive() == 1);
    REQUIRE (pool.countFading() == 2);

    // Releasing the pad releases only the note; the fades end on time even
    // though the release is two seconds long.
    pool.releasePad (0);
    juce::AudioBuffer<float> out (2, 512);
    out.clear();
    pool.renderAdding (out, 0, 512);
    REQUIRE (pool.countFading() == 0);
    REQUIRE (pool.countActive() == 1);    // the released note is in its tail
}

TEST_CASE ("fadeOut keeps the gain continuous when a fade is shortened", "[voice][declick]")
{
    Voice v;
    v.prepare (44100.0);
    VoiceConfig cfg;
    cfg.adsr = { 0.0f, 0.0f, 1.0f, 0.0f };
    v.start (makeDC (44100), 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 300);
    out.clear();
    v.fadeOut (200);
    v.renderAdding (out, 0, 100);         // halfway down
    v.fadeOut (20);                       // shorter: continues from there
    v.renderAdding (out, 100, 200);
    REQUIRE (largestStep (out, 0, 300) < 0.05f);
    REQUIRE_FALSE (v.isActive());
    v.fadeOut (10);                       // idle: no effect
    REQUIRE_FALSE (v.isFading());
}
