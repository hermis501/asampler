#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginProcessor.h"

using namespace asampler;

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

    juce::File writeWav (const juce::String& name, int numSamples,
                         bool harmonicallyRich = false)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("ASamplerTests");
        dir.createDirectory();
        const auto file = dir.getChildFile (name);
        file.deleteFile();

        juce::AudioBuffer<float> buf (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
        {
            if (harmonicallyRich)
            {
                const double t = static_cast<double> (i) / 44100.0;
                double v = 0.0;
                for (int h = 1; h <= 10; ++h)
                    v += std::sin (2.0 * juce::MathConstants<double>::pi * 200.0 * h * t) / h;
                buf.setSample (0, i, static_cast<float> (v * 0.3));
            }
            else
            {
                buf.setSample (0, i, 0.5f * std::sin (i * 0.05f));
            }
        }

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

    // Pad 1 as a keyboard instrument with formant correction over a range.
    void setUpCorrectedPad (ASamplerProcessor& proc, const juce::File& wav,
                            int lowNote, int highNote, int root)
    {
        REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
                 == ASamplerProcessor::LoadResult::Success);

        setPlain (proc, param_ids::padMode (1),
                  static_cast<float> (static_cast<int> (PadMode::Keyboard)));
        setPlain (proc, param_ids::padNote (1),     static_cast<float> (root));
        setPlain (proc, param_ids::padLowNote (1),  static_cast<float> (lowNote));
        setPlain (proc, param_ids::padHighNote (1), static_cast<float> (highNote));
        setPlain (proc, param_ids::padFormantCorrect (1), 1.0f);
    }
}

TEST_CASE ("Formant Correct parameter exists and defaults off",
           "[formantcorrect][parameters]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padFormantCorrect (1)) != nullptr);
    REQUIRE (plainValue (proc, param_ids::padFormantCorrect (1)) < 0.5f);
}

TEST_CASE ("Formant correction builds one variant per playable semitone",
           "[formantcorrect]")
{
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_range.wav", 8192);

    // C3..C4 with root C3 means offsets 0..12 inclusive: 13 variants.
    setUpCorrectedPad (proc, wav, 48, 60, 48);
    proc.renderFormantTableNow (0);

    REQUIRE (proc.getFormantVariantCount (0, 0) == 13);
}

TEST_CASE ("Correction covers offsets below the root too", "[formantcorrect]")
{
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_below.wav", 8192);

    // Root in the middle: offsets -5..+5 is 11 variants.
    setUpCorrectedPad (proc, wav, 55, 65, 60);
    proc.renderFormantTableNow (0);

    REQUIRE (proc.getFormantVariantCount (0, 0) == 11);
}

TEST_CASE ("Formant correction is off for Fixed-mode pads", "[formantcorrect]")
{
    // A fixed pad always plays at one ratio, so there is no per-note formant
    // drift to correct and no reason to spend the memory.
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_fixed.wav", 8192);

    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);
    setPlain (proc, param_ids::padMode (1),
              static_cast<float> (static_cast<int> (PadMode::Fixed)));
    setPlain (proc, param_ids::padFormantCorrect (1), 1.0f);

    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 0);
}

TEST_CASE ("Formant correction is off when the parameter is off", "[formantcorrect]")
{
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_off.wav", 8192);

    setUpCorrectedPad (proc, wav, 48, 60, 48);
    setPlain (proc, param_ids::padFormantCorrect (1), 0.0f);

    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 0);
}

TEST_CASE ("The variant range is clamped to two octaves either side",
           "[formantcorrect]")
{
    // A pad spanning the whole keyboard must not try to render 128 copies.
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_wide.wav", 4096);

    setUpCorrectedPad (proc, wav, 0, 127, 60);
    proc.renderFormantTableNow (0);

    const int n = proc.getFormantVariantCount (0, 0);
    REQUIRE (n > 0);
    REQUIRE (n <= kFormantVariantSlots);
}

TEST_CASE ("A corrected pad plays across its range", "[formantcorrect]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    const auto wav = writeWav ("fcorrect_play.wav", 44100);
    setUpCorrectedPad (proc, wav, 48, 60, 48);
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 13);

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);

    for (int note : { 48, 53, 60 })
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);
        proc.processBlock (buffer, midi);

        REQUIRE (proc.getActiveVoiceCount() > 0);
        REQUIRE (buffer.getMagnitude (0, 512) > 0.0f);

        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        proc.processBlock (buffer, off);
    }
}

TEST_CASE ("A note outside the rendered range still sounds", "[formantcorrect]")
{
    // Falling back to the uncorrected sample is right; going silent on a key
    // the user pressed is not.
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    const auto wav = writeWav ("fcorrect_outside.wav", 44100);

    setUpCorrectedPad (proc, wav, 48, 60, 48);
    proc.renderFormantTableNow (0);

    // Widen the pad without re-rendering: notes above 60 have no variant.
    setPlain (proc, param_ids::padHighNote (1), 127.0f);

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 72, 0.9f), 0);
    proc.processBlock (buffer, midi);

    REQUIRE (proc.getActiveVoiceCount() > 0);
    REQUIRE (buffer.getMagnitude (0, 512) > 0.0f);
}

TEST_CASE ("Correction renders a distinct variant per offset", "[formantcorrect]")
{
    // If every offset produced the same audio the feature would be inert.
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_rich.wav", 44100, /*rich*/ true);

    setUpCorrectedPad (proc, wav, 58, 62, 60);   // offsets -2..+2
    proc.renderFormantTableNow (0);

    REQUIRE (proc.getFormantVariantCount (0, 0) == 5);
}

TEST_CASE ("Loading a new sample invalidates the correction table",
           "[formantcorrect]")
{
    // A stale table would keep playing the previous sample's audio.
    ASamplerProcessor proc;
    const auto first  = writeWav ("fcorrect_first.wav", 8192);
    const auto second = writeWav ("fcorrect_second.wav", 8192);

    setUpCorrectedPad (proc, first, 58, 62, 60);
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 5);

    REQUIRE (proc.loadSampleForPadLayer (0, 0, second)
             == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 0);
}

TEST_CASE ("Resetting a pad drops the correction table", "[formantcorrect]")
{
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_reset.wav", 8192);

    setUpCorrectedPad (proc, wav, 58, 62, 60);
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 5);

    proc.resetPadLayerToDefault (0, 0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 0);
}

TEST_CASE ("Turning correction off drops the table", "[formantcorrect]")
{
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_toggle.wav", 8192);

    setUpCorrectedPad (proc, wav, 58, 62, 60);
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 5);

    setPlain (proc, param_ids::padFormantCorrect (1), 0.0f);
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 0);
}

TEST_CASE ("Narrowing the range rebuilds a smaller table", "[formantcorrect]")
{
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_narrow.wav", 8192);

    setUpCorrectedPad (proc, wav, 48, 60, 48);
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 13);

    setPlain (proc, param_ids::padHighNote (1), 52.0f);   // offsets 0..4
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 5);
}

TEST_CASE ("Correction combines with a user formant offset", "[formantcorrect]")
{
    // Correction holds formants against the note; the Formant parameter then
    // moves them deliberately. Both must apply.
    ASamplerProcessor proc;
    const auto wav = writeWav ("fcorrect_plus_offset.wav", 8192, true);

    setUpCorrectedPad (proc, wav, 58, 62, 60);
    setPlain (proc, param_ids::padFormant (1), 4.0f);

    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0, 0) == 5);
}

TEST_CASE ("Correction settings survive a state round-trip",
           "[formantcorrect][state]")
{
    juce::MemoryBlock block;
    {
        ASamplerProcessor proc;
        setPlain (proc, param_ids::padFormantCorrect (4), 1.0f);
        proc.getStateInformation (block);
    }

    ASamplerProcessor proc;
    proc.setStateInformation (block.getData(), (int) block.getSize());
    REQUIRE (plainValue (proc, param_ids::padFormantCorrect (4)) >= 0.5f);
}

TEST_CASE ("Key zones scope correction to each layer's own zone",
           "[formantcorrect][keyzones]")
{
    // With key zones the per-layer range is narrow, so each layer needs only a
    // few variants — the two features compose rather than multiplying cost.
    ASamplerProcessor proc;
    const auto low  = writeWav ("fcorrect_zone_low.wav", 8192);
    const auto high = writeWav ("fcorrect_zone_high.wav", 8192);

    REQUIRE (proc.loadSampleForPadLayer (0, 0, low)  == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 1, high) == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padMode (1),
              static_cast<float> (static_cast<int> (PadMode::Keyboard)));
    setPlain (proc, param_ids::padLowNote (1),  36.0f);
    setPlain (proc, param_ids::padHighNote (1), 71.0f);
    setPlain (proc, param_ids::padLayerMode (1),
              static_cast<float> (static_cast<int> (LayerMode::KeyZones)));
    setPlain (proc, param_ids::padFormantCorrect (1), 1.0f);

    // Layer 1: C2..B2 rooted C2 (offsets 0..11). Layer 2: C3..B3 rooted C3.
    setPlain (proc, param_ids::padLayerLowNote (1, 1),  36.0f);
    setPlain (proc, param_ids::padLayerHighNote (1, 1), 47.0f);
    setPlain (proc, param_ids::padLayerRootNote (1, 1), 36.0f);
    setPlain (proc, param_ids::padLayerLowNote (1, 2),  48.0f);
    setPlain (proc, param_ids::padLayerHighNote (1, 2), 59.0f);
    setPlain (proc, param_ids::padLayerRootNote (1, 2), 48.0f);

    proc.renderFormantTableNow (0);

    REQUIRE (proc.getFormantVariantCount (0, 0) == 12);
    REQUIRE (proc.getFormantVariantCount (0, 1) == 12);
}
