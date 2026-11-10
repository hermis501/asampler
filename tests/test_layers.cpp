// Layers beyond the basics: eight per pad, per-layer velocity ranges that can
// stack, release-trigger layers, per-layer tuning, and the two per-pad length
// controls (Speed, Keep Length) that render in the background.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

using namespace asampler;
using Catch::Approx;

namespace
{
    constexpr float kEqualPower = 0.7071068f;   // centre pan

    void setParam (ASamplerProcessor& proc, const juce::String& id, float value)
    {
        auto* p = proc.getState().getParameter (id);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    juce::File writeWav (const juce::String& stem, const juce::AudioBuffer<float>& buffer)
    {
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("asampler_layers_" + stem + ".wav");
        file.deleteFile();

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
        REQUIRE (os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (os.get(), 44100.0, (unsigned) buffer.getNumChannels(), 24, {}, 0));
        REQUIRE (writer != nullptr);
        os.release();
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return file;
    }

    juce::File writeConstant (const juce::String& stem, float amplitude, int numSamples = 4096)
    {
        juce::AudioBuffer<float> b (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            b.setSample (0, i, amplitude);
        return writeWav (stem, b);
    }

    juce::File writeSine (const juce::String& stem, double seconds, float freq, float amp = 0.5f)
    {
        const int n = juce::roundToInt (seconds * 44100.0);
        juce::AudioBuffer<float> b (1, n);
        for (int i = 0; i < n; ++i)
            b.setSample (0, i, amp * std::sin (juce::MathConstants<float>::twoPi * freq * (float) i / 44100.0f));
        return writeWav (stem, b);
    }

    // Flat, centred, full-sustain pad, so a render's peak is the sum of the
    // sounding samples' amplitudes times the pan law.
    void makeFlat (ASamplerProcessor& proc, int padOneBased, bool fixedVelocity = true)
    {
        if (fixedVelocity)
            setParam (proc, param_ids::padVelocityCurve (padOneBased), static_cast<float> (VelocityCurve::Fixed));
        setParam (proc, param_ids::padAttack  (padOneBased), 0.0f);
        setParam (proc, param_ids::padDecay   (padOneBased), 0.0f);
        setParam (proc, param_ids::padSustain (padOneBased), 1.0f);
        setParam (proc, param_ids::padRelease (padOneBased), 0.0f);
        setParam (proc, param_ids::padPan     (padOneBased), 0.0f);
    }

    float peakOf (const juce::AudioBuffer<float>& b)
    {
        return b.getMagnitude (0, 0, b.getNumSamples());
    }

    float renderNote (ASamplerProcessor& proc, int note, float velocity, int numSamples = 512)
    {
        juce::AudioBuffer<float> buf (2, numSamples);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), 0);
        proc.processBlock (buf, midi);
        return peakOf (buf);
    }

    float renderNoteOff (ASamplerProcessor& proc, int note, int numSamples = 512)
    {
        juce::AudioBuffer<float> buf (2, numSamples);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        proc.processBlock (buf, midi);
        return peakOf (buf);
    }

    // Render `seconds` after a note-on into one mono buffer (left channel).
    std::vector<float> renderSeconds (ASamplerProcessor& proc, int note, double seconds)
    {
        std::vector<float> out;
        const int total = juce::roundToInt (seconds * 44100.0);
        bool first = true;
        while ((int) out.size() < total)
        {
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            if (first)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
            first = false;
            proc.processBlock (buf, midi);
            for (int i = 0; i < 512; ++i)
                out.push_back (buf.getSample (0, i));
        }

        // Let go, so the next render starts from silence.
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        proc.processBlock (buf, midi);
        return out;
    }

    // Seconds until the signal stays below `threshold` for good.
    double audibleSeconds (const std::vector<float>& x, float threshold = 0.01f)
    {
        for (int i = (int) x.size() - 1; i >= 0; --i)
            if (std::abs (x[(std::size_t) i]) > threshold)
                return (i + 1) / 44100.0;
        return 0.0;
    }

    double zeroCrossingsPerSecond (const std::vector<float>& x, int from, int to)
    {
        int crossings = 0;
        for (int i = from + 1; i < to; ++i)
            if ((x[(std::size_t) i - 1] < 0.0f) != (x[(std::size_t) i] < 0.0f))
                ++crossings;
        return crossings / ((to - from) / 44100.0);
    }
}

TEST_CASE ("A pad has eight layers and the fifth to eighth play", "[plugin][layers]")
{
    REQUIRE (kNumLayers == 8);

    auto f = writeConstant ("l8", 0.4f);
    ASamplerProcessor proc;
    makeFlat (proc, 1);
    proc.prepareToPlay (44100.0, 512);

    // Only layer 8 loaded: every velocity falls back to it.
    REQUIRE (proc.loadSampleForPadLayer (0, 7, f) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.getPadLayerSampleDisplayName (0, 7) == f.getFileName());
    REQUIRE (renderNote (proc, 36, 0.2f) == Approx (0.4f * kEqualPower).margin (0.01f));

    // Edit Layer reaches it too.
    setParam (proc, param_ids::editLayer, 7.0f);
    REQUIRE (proc.getCurrentEditLayer() == 7);
    f.deleteFile();
}

TEST_CASE ("Velocity ranges switch layers and overlapping ranges stack", "[plugin][layers][velocity]")
{
    auto a = writeConstant ("va", 0.1f);
    auto b = writeConstant ("vb", 0.2f);

    ASamplerProcessor proc;
    makeFlat (proc, 1);
    proc.prepareToPlay (44100.0, 512);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, a) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 1, b) == ASamplerProcessor::LoadResult::Success);

    SECTION ("Split at 64")
    {
        setParam (proc, param_ids::padLayerVelLow  (1, 1), 0.0f);
        setParam (proc, param_ids::padLayerVelHigh (1, 1), 63.0f);
        setParam (proc, param_ids::padLayerVelLow  (1, 2), 64.0f);
        setParam (proc, param_ids::padLayerVelHigh (1, 2), 127.0f);

        REQUIRE (renderNote (proc, 36, 0.3f) == Approx (0.1f * kEqualPower).margin (0.01f));
        renderNoteOff (proc, 36);
        REQUIRE (renderNote (proc, 36, 0.9f) == Approx (0.2f * kEqualPower).margin (0.01f));
    }

    SECTION ("Both cover every velocity: they play together")
    {
        for (int L = 1; L <= 2; ++L)
        {
            setParam (proc, param_ids::padLayerVelLow  (1, L), 0.0f);
            setParam (proc, param_ids::padLayerVelHigh (1, L), 127.0f);
        }
        REQUIRE (proc.getState().getParameter (param_ids::padMaxVoices (1)) != nullptr);
        setParam (proc, param_ids::padMaxVoices (1), 1.0f);   // a mono pad keeps both stacked layers

        REQUIRE (renderNote (proc, 36, 0.5f) == Approx (0.3f * kEqualPower).margin (0.01f));
        REQUIRE (proc.getActiveVoiceCount() == 2);

        // The next hit replaces both rather than piling up.
        renderNote (proc, 36, 0.5f);
        REQUIRE (proc.getActiveVoiceCount() == 2);
    }

    a.deleteFile();
    b.deleteFile();
}

TEST_CASE ("Default velocity bands of layers 1 to 4 are the old equal bands", "[plugin][layers][velocity]")
{
    ASamplerProcessor proc;
    auto value = [&proc] (const juce::String& id)
    {
        auto* p = proc.getState().getParameter (id);
        return juce::roundToInt (p->convertFrom0to1 (p->getValue()));
    };

    for (int L = 1; L <= 4; ++L)
    {
        CAPTURE (L);
        REQUIRE (value (param_ids::padLayerVelLow  (1, L)) == (L - 1) * 32);
        REQUIRE (value (param_ids::padLayerVelHigh (1, L)) == L * 32 - 1);
    }
    for (int L = 5; L <= 8; ++L)
    {
        REQUIRE (value (param_ids::padLayerVelLow  (1, L)) == 0);
        REQUIRE (value (param_ids::padLayerVelHigh (1, L)) == 127);
    }
}

TEST_CASE ("A release layer plays at note-off with the note-on velocity", "[plugin][layers][release]")
{
    auto press   = writeConstant ("rp", 0.1f, 44100);
    auto release = writeConstant ("rr", 0.4f, 2048);

    ASamplerProcessor proc;
    makeFlat (proc, 1, /*fixedVelocity*/ false);
    setParam (proc, param_ids::padVelocityCurve (1), static_cast<float> (VelocityCurve::Linear));
    proc.prepareToPlay (44100.0, 512);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, press)   == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 1, release) == ASamplerProcessor::LoadResult::Success);
    for (int L = 1; L <= 2; ++L)
    {
        setParam (proc, param_ids::padLayerVelLow  (1, L), 0.0f);
        setParam (proc, param_ids::padLayerVelHigh (1, L), 127.0f);
    }
    setParam (proc, param_ids::padLayerTrigger (1, 2), 1.0f);   // Release

    // Press: only the press layer, at half velocity.
    const float pressed = renderNote (proc, 36, 0.5f);
    const float pressGain = pressed / (0.1f * kEqualPower);
    REQUIRE (pressGain > 0.2f);
    REQUIRE (pressGain < 0.8f);

    // Release: the press layer stops (release 0) and the release layer plays
    // at the same velocity gain.
    const float released = renderNoteOff (proc, 36);
    REQUIRE (released == Approx (0.4f * kEqualPower * pressGain).margin (0.02f));

    press.deleteFile();
    release.deleteFile();
}

TEST_CASE ("A pad holding only a release layer is silent until the key goes up", "[plugin][layers][release]")
{
    auto release = writeConstant ("ronly", 0.3f, 1024);

    ASamplerProcessor proc;
    makeFlat (proc, 1);
    proc.prepareToPlay (44100.0, 512);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, release) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padLayerTrigger (1, 1), 1.0f);

    REQUIRE (renderNote (proc, 36, 0.8f) == 0.0f);
    REQUIRE (renderNoteOff (proc, 36) == Approx (0.3f * kEqualPower).margin (0.01f));
    release.deleteFile();
}

TEST_CASE ("A release voice plays through once even on a looping pad", "[plugin][layers][release]")
{
    auto release = writeConstant ("rloop", 0.3f, 2000);

    ASamplerProcessor proc;
    makeFlat (proc, 1);
    setParam (proc, param_ids::padLoopMode (1), static_cast<float> (LoopMode::Forward));
    proc.prepareToPlay (44100.0, 512);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, release) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padLayerTrigger (1, 1), 1.0f);

    renderNote (proc, 36, 0.8f);
    renderNoteOff (proc, 36);
    for (int i = 0; i < 20; ++i)
    {
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer none;
        proc.processBlock (buf, none);
    }
    REQUIRE (proc.getActiveVoiceCount() == 0);
    release.deleteFile();
}

TEST_CASE ("Layer transpose and fine tune move that layer's pitch", "[plugin][layers][tuning]")
{
    auto sine = writeSine ("tune", 1.0, 441.0f);

    ASamplerProcessor proc;
    makeFlat (proc, 1);
    proc.prepareToPlay (44100.0, 512);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, sine) == ASamplerProcessor::LoadResult::Success);

    const auto base = renderSeconds (proc, 36, 0.3);
    const double f0 = zeroCrossingsPerSecond (base, 2000, 12000);

    setParam (proc, param_ids::padLayerTranspose (1, 1), 12.0f);
    const auto up = renderSeconds (proc, 36, 0.3);
    REQUIRE (zeroCrossingsPerSecond (up, 2000, 12000) / f0 == Approx (2.0).margin (0.03));

    setParam (proc, param_ids::padLayerTranspose (1, 1), 0.0f);
    setParam (proc, param_ids::padLayerFine (1, 1), 100.0f);   // +100 cents = +1 semitone
    const auto cents = renderSeconds (proc, 36, 0.3);
    REQUIRE (zeroCrossingsPerSecond (cents, 2000, 12000) / f0 == Approx (std::pow (2.0, 1.0 / 12.0)).margin (0.02));

    sine.deleteFile();
}

TEST_CASE ("Speed changes a pad's length without changing its pitch", "[plugin][stretch][speed]")
{
    auto sine = writeSine ("speed", 1.0, 441.0f);

    ASamplerProcessor proc;
    makeFlat (proc, 1);
    proc.prepareToPlay (44100.0, 512);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, sine) == ASamplerProcessor::LoadResult::Success);

    const auto plain = renderSeconds (proc, 36, 1.5);
    const double plainLength = audibleSeconds (plain);
    REQUIRE (plainLength == Approx (1.0).margin (0.02));

    setParam (proc, param_ids::padSpeed (1), 100.0f);   // twice as fast
    proc.renderStretchNow (0);
    REQUIRE (proc.isPadRendered (0));

    const auto fast = renderSeconds (proc, 36, 1.5);
    REQUIRE (audibleSeconds (fast) == Approx (0.5).margin (0.05));
    REQUIRE (zeroCrossingsPerSecond (fast, 2000, 12000) / zeroCrossingsPerSecond (plain, 2000, 12000)
             == Approx (1.0).margin (0.03));

    // Its text says what it does.
    auto* p = proc.getState().getParameter (param_ids::padSpeed (1));
    REQUIRE (p->getCurrentValueAsText() == "+100%, 2.00x speed");
    sine.deleteFile();
}

TEST_CASE ("Keep Length holds the length of notes played above the root", "[plugin][keeplength]")
{
    auto sine = writeSine ("keep", 1.0, 220.0f);

    ASamplerProcessor proc;
    makeFlat (proc, 1);
    setParam (proc, param_ids::padMode (1), static_cast<float> (PadMode::Keyboard));
    setParam (proc, param_ids::padNote (1), 60.0f);
    setParam (proc, param_ids::padLowNote (1), 48.0f);
    setParam (proc, param_ids::padHighNote (1), 72.0f);
    proc.prepareToPlay (44100.0, 512);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, sine) == ASamplerProcessor::LoadResult::Success);

    // Without it, an octave up plays for half the time.
    REQUIRE (audibleSeconds (renderSeconds (proc, 72, 1.5)) == Approx (0.5).margin (0.03));

    setParam (proc, param_ids::padKeepLength (1), 1.0f);
    proc.renderFormantTableNow (0);
    REQUIRE (proc.getFormantVariantCount (0) == 25);   // -12 .. +12

    const auto octaveUp = renderSeconds (proc, 72, 1.5);
    REQUIRE (audibleSeconds (octaveUp) == Approx (1.0).margin (0.06));

    // ...and it is still an octave up.
    const auto root = renderSeconds (proc, 60, 1.5);
    REQUIRE (zeroCrossingsPerSecond (octaveUp, 4000, 20000) / zeroCrossingsPerSecond (root, 4000, 20000)
             == Approx (2.0).margin (0.05));
    sine.deleteFile();
}

TEST_CASE ("Swapping a layer's audio drops a stale render", "[plugin][stretch][layers]")
{
    auto a = writeSine ("stale_a", 0.5, 441.0f);
    auto b = writeSine ("stale_b", 0.5, 330.0f);

    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, a) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (1, 0, b) == ASamplerProcessor::LoadResult::Success);

    setParam (proc, param_ids::padSpeed (1), 50.0f);
    proc.renderStretchNow (0);
    REQUIRE (proc.isPadRendered (0));

    SECTION ("Paste")
    {
        REQUIRE (proc.copyPad (1));
        REQUIRE (proc.pastePad (0, true));
        REQUIRE_FALSE (proc.isPadRendered (0));
    }

    SECTION ("Chop")
    {
        REQUIRE (proc.chopAcrossPads (1, 2) == ASamplerProcessor::ChopResult::Success);
        REQUIRE_FALSE (proc.isPadRendered (0));
    }

    a.deleteFile();
    b.deleteFile();
}
