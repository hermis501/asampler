#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginProcessor.h"

using namespace asampler;

// Formant correction indexes its variant table by the voice's *total* pitch
// offset, not just the note interval. These tests pin down each contributor:
// the Pitch parameter, Vel->Pitch, and Repitch-mode tempo sync.

namespace
{
    float plainValue (ASamplerProcessor& proc, const juce::String& id)
    {
        auto* p = proc.getState().getParameter (id);
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    }

    void setPlain (ASamplerProcessor& proc, const juce::String& id, float v)
    {
        auto value = proc.getState().getParameterAsValue (id);
        if (! value.getValue().isVoid())
        {
            value.setValue (v);
            return;
        }

        if (auto* p = proc.getState().getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    juce::File writeWav (const juce::String& name, int numSamples = 8192)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("ASamplerTests");
        dir.createDirectory();
        const auto file = dir.getChildFile (name);
        file.deleteFile();

        juce::AudioBuffer<float> buf (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (0, i, 0.5f * std::sin (i * 0.05f));

        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        if (stream == nullptr) return {};
        std::unique_ptr<juce::AudioFormatWriter> writer (
            fmt.createWriterFor (stream.release(), 44100.0, 1, 16, {}, 0));
        if (writer == nullptr) return {};
        writer->writeFromAudioSampleBuffer (buf, 0, numSamples);
        writer.reset();
        return file;
    }

    // A Fixed-mode pad with correction on. Fixed mode contributes no note
    // interval, so whatever table appears is driven purely by the other pitch
    // sources — which is exactly what these tests want to isolate.
    void setUpFixedPad (ASamplerProcessor& proc, const juce::File& wav)
    {
        REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
                 == ASamplerProcessor::LoadResult::Success);

        setPlain (proc, param_ids::padMode (1),
                  static_cast<float> (static_cast<int> (PadMode::Fixed)));
        setPlain (proc, param_ids::padFormantCorrect (1), 1.0f);
    }
}

TEST_CASE ("A Fixed pad at root pitch needs no table", "[formantpitch]")
{
    // Nothing shifts the pitch, so there is nothing to correct and the single
    // stretch/formant render already covers it.
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_none.wav"));

    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 0);
}

TEST_CASE ("The Pitch parameter drives formant correction", "[formantpitch]")
{
    // Previously only the note interval was corrected, so a pad transposed by
    // the Pitch knob kept its formants dragged along. Now it does not.
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_up.wav"));

    setPlain (proc, param_ids::padPitch (1), 7.0f);
    proc.renderFormantTableNow (0);

    REQUIRE (proc.getFormantVariantCount (0, 0) == 1);
}

TEST_CASE ("A negative Pitch offset is covered too", "[formantpitch]")
{
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_down.wav"));

    setPlain (proc, param_ids::padPitch (1), -5.0f);
    proc.renderFormantTableNow (0);

    REQUIRE (proc.getFormantVariantCount (0, 0) == 1);
}

TEST_CASE ("A fractional Pitch value covers both rounding neighbours",
           "[formantpitch]")
{
    // Pitch is continuous, so the table brackets it: the lookup rounds to
    // whichever neighbour is nearer, leaving at most half a semitone of
    // residual formant error.
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_frac.wav"));

    setPlain (proc, param_ids::padPitch (1), 7.4f);
    proc.renderFormantTableNow (0);

    REQUIRE (proc.getFormantVariantCount (0, 0) == 2);   // offsets 7 and 8
}

TEST_CASE ("Vel to Pitch widens the corrected range", "[formantpitch]")
{
    // Vel->Pitch varies per hit, so the table has to span zero to its full
    // amount rather than a single value.
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_vel.wav"));

    setPlain (proc, param_ids::padVelToPitch (1), 5.0f);
    proc.renderFormantTableNow (0);

    REQUIRE (proc.getFormantVariantCount (0, 0) == 6);   // offsets 0..5
}

TEST_CASE ("A negative Vel to Pitch spans downward", "[formantpitch]")
{
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_velneg.wav"));

    setPlain (proc, param_ids::padVelToPitch (1), -3.0f);
    proc.renderFormantTableNow (0);

    REQUIRE (proc.getFormantVariantCount (0, 0) == 4);   // offsets -3..0
}

TEST_CASE ("Repitch tempo sync is corrected", "[formantpitch]")
{
    // Repitch mode speeds the sample up to match tempo, which drags formants
    // with it exactly like any other pitch change.
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_tempo.wav"));

    setPlain (proc, param_ids::padStretchMode (1),
              static_cast<float> (static_cast<int> (StretchMode::Repitch)));
    setPlain (proc, param_ids::padTempoSync (1), 1.0f);
    // No transport in tests, so host tempo falls back to 120. An original of
    // 60 BPM means playing at 2x: exactly +12 semitones.
    setPlain (proc, param_ids::padOriginalBpm (1), 60.0f);

    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 1);
}

TEST_CASE ("Stretch-mode tempo sync adds no pitch to correct", "[formantpitch]")
{
    // Stretch mode fits the tempo by changing duration, not pitch, so there is
    // no extra offset for the table to cover.
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_stretchmode.wav"));

    setPlain (proc, param_ids::padStretchMode (1),
              static_cast<float> (static_cast<int> (StretchMode::Stretch)));
    setPlain (proc, param_ids::padTempoSync (1), 1.0f);
    setPlain (proc, param_ids::padOriginalBpm (1), 60.0f);

    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 0);
}

TEST_CASE ("Pitch and note interval combine into one range", "[formantpitch]")
{
    // A keyboard pad transposed by the Pitch knob needs the union, not either
    // one alone.
    ASamplerProcessor proc;

    REQUIRE (proc.loadSampleForPadLayer (0, 0, writeWav ("fpitch_combined.wav"))
             == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padMode (1),
              static_cast<float> (static_cast<int> (PadMode::Keyboard)));
    setPlain (proc, param_ids::padNote (1),     48.0f);
    setPlain (proc, param_ids::padLowNote (1),  48.0f);
    setPlain (proc, param_ids::padHighNote (1), 54.0f);   // intervals 0..6
    setPlain (proc, param_ids::padPitch (1),    4.0f);    // shifted to 4..10
    setPlain (proc, param_ids::padFormantCorrect (1), 1.0f);

    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 7);
}

TEST_CASE ("A pitched, corrected pad still sounds", "[formantpitch]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    setUpFixedPad (proc, writeWav ("fpitch_play.wav", 44100));
    setPlain (proc, param_ids::padPitch (1), 7.0f);
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 1);

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 0.9f), 0);
    proc.processBlock (buffer, midi);

    REQUIRE (proc.getActiveVoiceCount() > 0);
    REQUIRE (buffer.getMagnitude (0, 512) > 0.0f);
}

TEST_CASE ("A pitch beyond the table range still sounds", "[formantpitch]")
{
    // The lookup clamps into the table; anything past it falls back to the
    // uncorrected sample rather than producing silence.
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    setUpFixedPad (proc, writeWav ("fpitch_far.wav", 44100));
    setPlain (proc, param_ids::padPitch (1), 5.0f);
    proc.renderFormantTableNow (0);

    // Move Pitch far past what was rendered, without re-rendering.
    setPlain (proc, param_ids::padPitch (1), 23.0f);

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 0.9f), 0);
    proc.processBlock (buffer, midi);

    REQUIRE (proc.getActiveVoiceCount() > 0);
    REQUIRE (buffer.getMagnitude (0, 512) > 0.0f);
}

TEST_CASE ("Changing Pitch rebuilds the table for the new range",
           "[formantpitch]")
{
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_rebuild.wav"));

    setPlain (proc, param_ids::padVelToPitch (1), 4.0f);   // offsets 0..4
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 5);

    setPlain (proc, param_ids::padVelToPitch (1), 8.0f);   // offsets 0..8
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 9);
}

TEST_CASE ("An extreme Pitch value stays within the variant cap",
           "[formantpitch]")
{
    ASamplerProcessor proc;
    setUpFixedPad (proc, writeWav ("fpitch_extreme.wav", 4096));

    setPlain (proc, param_ids::padPitch (1), 24.0f);
    setPlain (proc, param_ids::padVelToPitch (1), -24.0f);

    proc.renderFormantTableNow (0);

    const int n = proc.getFormantVariantCount (0, 0);
    REQUIRE (n > 0);
    REQUIRE (n <= kFormantVariantSlots);
}
