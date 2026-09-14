#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"

using namespace asampler;

// The SAPI5 path needs a Windows speech voice installed. On a normal Windows
// dev box (Microsoft David/Zira ship by default) the Success path runs for
// real; on a headless/voiceless environment it returns SynthFailed. Tests that
// exercise real synthesis therefore accept either "it worked" or "no voice",
// and only assert the richer post-conditions when synthesis actually succeeded.

TEST_CASE ("Vocal Target Pad parameter resolves and defaults to Pad 1",
           "[plugin][vocal][parameters]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getState().getRawParameterValue (param_ids::vocalTargetPad) != nullptr);
    REQUIRE (proc.getCurrentVocalTargetPad() == 0);   // 0-based; "Pad 1"
}

TEST_CASE ("Vocal generation rejects empty text", "[plugin][vocal]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.synthesizeVocalForPad (0, "   ")
             == ASamplerProcessor::VocalResult::EmptyText);
    REQUIRE (proc.synthesizeVocalForPad (0, "")
             == ASamplerProcessor::VocalResult::EmptyText);
}

TEST_CASE ("Vocal generation rejects out-of-range pads", "[plugin][vocal]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.synthesizeVocalForPad (-1, "hello")
             == ASamplerProcessor::VocalResult::InvalidPad);
    REQUIRE (proc.synthesizeVocalForPad (kNumPads, "hello")
             == ASamplerProcessor::VocalResult::InvalidPad);
}

TEST_CASE ("Vocal generation loads a spoken sample onto the pad", "[plugin][vocal]")
{
    ASamplerProcessor proc;
    const auto r = proc.synthesizeVocalForPad (2, "one two three");

    if (r == ASamplerProcessor::VocalResult::Success)
    {
        // A real sample now sits on the pad — no longer the built-in default.
        REQUIRE (proc.getPadSampleDisplayName (2) != juce::String ("default"));
    }
    else
    {
        // No speech voice available in this environment.
        REQUIRE (r == ASamplerProcessor::VocalResult::SynthFailed);
    }
}

TEST_CASE ("Vocal spread flips the target pad into Keyboard mode", "[plugin][vocal]")
{
    ASamplerProcessor proc;
    const auto r = proc.synthesizeVocalAndSpread (1, "vocal chop", 48, 72);

    if (r == ASamplerProcessor::VocalResult::Success)
    {
        auto& state = proc.getState();
        const int mode = juce::roundToInt (
            state.getRawParameterValue (param_ids::padMode (2))->load());  // pad index 1 -> id 2
        REQUIRE (mode == static_cast<int> (PadMode::Keyboard));

        const int lo = juce::roundToInt (
            state.getRawParameterValue (param_ids::padLowNote (2))->load());
        const int hi = juce::roundToInt (
            state.getRawParameterValue (param_ids::padHighNote (2))->load());
        REQUIRE (lo == 48);
        REQUIRE (hi == 72);
    }
    else
    {
        REQUIRE (r == ASamplerProcessor::VocalResult::SynthFailed);
    }
}
