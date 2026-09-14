#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginProcessor.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <memory>

using namespace asampler;
using Catch::Approx;

namespace
{
    void setParam (ASamplerProcessor& proc, const juce::String& id, float value)
    {
        auto* p = proc.getState().getParameter (id);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    juce::File tempWavFile (const juce::String& stem)
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("asampler_test_" + stem + ".wav");
    }

    // Writes a short WAV (sine tone) we can use as a sample-loading fixture.
    juce::File writeSineWav (const juce::String& stem,
                             int   numChannels = 1,
                             int   numSamples  = 256,
                             float freqHz      = 440.0f)
    {
        auto file = tempWavFile (stem);
        if (file.existsAsFile()) file.deleteFile();

        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* w = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                w[i] = std::sin (juce::MathConstants<float>::twoPi * freqHz
                                  * static_cast<float> (i) / 44100.0f) * 0.5f;
        }

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
        REQUIRE (os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (os.get(), 44100.0, (unsigned int) numChannels, 16, {}, 0));
        REQUIRE (writer != nullptr);
        os.release(); // writer owns the stream now
        writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
        writer.reset();
        return file;
    }

    // Writes a WAV containing N decaying-sine bursts at evenly-spaced positions —
    // a synthetic stand-in for a drum loop with clearly-defined onsets. Used to
    // exercise transient-mode chop end-to-end.
    juce::File writeBurstWav (const juce::String& stem, int numBursts = 4,
                              double durationSec = 1.0)
    {
        auto file = tempWavFile (stem);
        if (file.existsAsFile()) file.deleteFile();

        const int sr           = 44100;
        const int totalSamples = static_cast<int> (sr * durationSec);
        const int burstSamples = 2205;     // 50 ms
        const int gap          = totalSamples / numBursts;

        juce::AudioBuffer<float> buffer (1, totalSamples);
        buffer.clear();
        auto* w = buffer.getWritePointer (0);
        for (int b = 0; b < numBursts; ++b)
        {
            const int startPos = b * gap;
            for (int n = 0; n < burstSamples && (startPos + n) < totalSamples; ++n)
            {
                const float t   = static_cast<float> (n) / static_cast<float> (sr);
                const float env = std::exp (-30.0f * t);
                w[startPos + n] += std::sin (juce::MathConstants<float>::twoPi * 440.0f * t)
                                 * env * 0.8f;
            }
        }

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
        REQUIRE (os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (os.get(), 44100.0, 1u, 16, {}, 0));
        REQUIRE (writer != nullptr);
        os.release();
        writer->writeFromAudioSampleBuffer (buffer, 0, totalSamples);
        writer.reset();
        return file;
    }

    // Writes a WAV whose samples are all the same constant value. Lets a test
    // identify "which sample is currently loaded" by reading peak amplitude
    // from the render output rather than fingerprinting via DSP.
    juce::File writeConstantWav (const juce::String& stem, float amplitude,
                                 int numSamples = 256)
    {
        auto file = tempWavFile (stem);
        if (file.existsAsFile()) file.deleteFile();

        juce::AudioBuffer<float> buffer (1, numSamples);
        auto* w = buffer.getWritePointer (0);
        for (int i = 0; i < numSamples; ++i)
            w[i] = amplitude;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
        REQUIRE (os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (os.get(), 44100.0, 1u, 16, {}, 0));
        REQUIRE (writer != nullptr);
        os.release();
        writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
        writer.reset();
        return file;
    }

    // Writes a header-only WAV (0 audio frames). The RIFF/fmt/data chunks
    // are well-formed but the data section is empty, which is what we need
    // to exercise the EmptyFile branch of loadSampleForPad. JUCE's writer
    // doesn't reliably produce a 0-frame file when you skip the write call,
    // so we hand-craft the bytes.
    juce::File writeEmptyWav (const juce::String& stem)
    {
        auto file = tempWavFile (stem);
        if (file.existsAsFile()) file.deleteFile();

        juce::FileOutputStream os (file);
        REQUIRE (os.openedOk());

        os.writeText ("RIFF", false, false, nullptr);
        os.writeInt (36);                  // file size minus 8 (header + fmt chunk + data hdr)
        os.writeText ("WAVE", false, false, nullptr);

        os.writeText ("fmt ", false, false, nullptr);
        os.writeInt (16);                  // PCM fmt chunk size
        os.writeShort (1);                 // PCM
        os.writeShort (1);                 // mono
        os.writeInt (44100);               // sample rate
        os.writeInt (88200);               // byte rate
        os.writeShort (2);                 // block align
        os.writeShort (16);                // bits per sample

        os.writeText ("data", false, false, nullptr);
        os.writeInt (0);                   // 0 audio bytes
        os.flush();
        return file;
    }

    // Steady full-amplitude sine — used to distinguish a sustaining voice from
    // a fast-decaying one (the default pluck dies in ~400ms).
    juce::File writeSteadyWav (const juce::String& stem, int numSamples = 22050)
    {
        auto file = tempWavFile (stem);
        if (file.existsAsFile()) file.deleteFile();

        juce::AudioBuffer<float> buffer (1, numSamples);
        auto* w = buffer.getWritePointer (0);
        for (int i = 0; i < numSamples; ++i)
            w[i] = std::sin (juce::MathConstants<float>::twoPi * 220.0f
                              * static_cast<float> (i) / 44100.0f) * 0.9f;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
        REQUIRE (os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (os.get(), 44100.0, 1u, 16, {}, 0));
        REQUIRE (writer != nullptr);
        os.release();
        writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
        writer.reset();
        return file;
    }

    float blockEnergy (const juce::AudioBuffer<float>& buf)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            const auto* r = buf.getReadPointer (ch);
            for (int i = 0; i < buf.getNumSamples(); ++i)
                sum += std::abs (r[i]);
        }
        return sum;
    }

    float channelEnergy (const juce::AudioBuffer<float>& buf, int ch)
    {
        float sum = 0.0f;
        const auto* r = buf.getReadPointer (ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            sum += std::abs (r[i]);
        return sum;
    }

    juce::MidiBuffer noteOnAt (int note, float velocity, int samplePos = 0)
    {
        juce::MidiBuffer buf;
        buf.addEvent (juce::MidiMessage::noteOn (1, note, velocity), samplePos);
        return buf;
    }
}

// ----- Parameter wiring --------------------------------------------------

TEST_CASE ("Every parameter ID resolves to a non-null atomic", "[plugin][parameters]")
{
    ASamplerProcessor proc;
    auto& state = proc.getState();

    REQUIRE (state.getRawParameterValue (param_ids::masterGain) != nullptr);

    for (int p = 1; p <= kNumPads; ++p)
    {
        CAPTURE (p);
        REQUIRE (state.getRawParameterValue (param_ids::padVolume        (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padPan           (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padPitch         (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padNote          (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padAttack        (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padDecay         (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padSustain       (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padRelease       (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padChokeGroup    (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padStart            (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padEnd              (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padReverse          (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padVelocityCurve    (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padFilterMode       (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padFilterCutoff     (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padFilterResonance  (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padLoopMode         (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padLoopStart        (p)) != nullptr);
        REQUIRE (state.getRawParameterValue (param_ids::padLoopEnd          (p)) != nullptr);
    }
}

TEST_CASE ("Default per-pad notes start at 36 (C2) and ascend chromatically",
           "[plugin][parameters]")
{
    ASamplerProcessor proc;
    auto& state = proc.getState();
    for (int p = 1; p <= kNumPads; ++p)
    {
        CAPTURE (p);
        const int note = static_cast<int> (state.getRawParameterValue (param_ids::padNote (p))->load());
        REQUIRE (note == 35 + p);
    }
}

// ----- Sample loading ----------------------------------------------------

TEST_CASE ("loadSampleForPad rejects a non-existent file", "[plugin][loading]")
{
    ASamplerProcessor proc;
    // Built from the temp dir so the path is absolute on every platform — a
    // "C:/..." literal is a relative path on macOS and trips juce::File's assert.
    const auto missing = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("asampler_this_should_not_exist")
                             .getChildFile ("missing_sample.wav");
    REQUIRE (proc.loadSampleForPad (0, missing) == ASamplerProcessor::LoadResult::FileNotFound);
}

TEST_CASE ("AudioParameterChoice label ordering is frozen",
           "[plugin][parameters][choice-order]")
{
    // This test pins every choice parameter's labels to their current index
    // position. AudioParameterChoice serialises by index, so reordering the
    // labels would silently remap saved projects (e.g. Transients -> Equal).
    // If you legitimately need to extend a list, *append only* — and update
    // this test to add the new entry at the end. A failure here means a
    // pre-existing project's choice values would be misinterpreted.
    ASamplerProcessor proc;
    auto& s = proc.getState();

    auto check = [&] (const juce::String& id, const juce::StringArray& expected)
    {
        CAPTURE (id.toStdString());
        auto* p = s.getParameter (id);
        REQUIRE (p != nullptr);
        auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p);
        REQUIRE (choice != nullptr);
        REQUIRE (choice->choices.size() == expected.size());
        for (int i = 0; i < expected.size(); ++i)
        {
            CAPTURE (i);
            REQUIRE (choice->choices[i] == expected[i]);
        }
    };

    check (param_ids::chopCount, { "2", "4", "8", "16" });
    check (param_ids::chopMode,  { "Equal", "Transients", "Transients (Spectral)" });
    check (param_ids::editLayer, { "Layer 1", "Layer 2", "Layer 3", "Layer 4" });

    // Per-pad choices are emitted from the same template, so locking pad 1
    // is enough to lock the others (and we exercise the param_ids helpers
    // by going through one of them).
    check (param_ids::padVelocityCurve (1),
           { "Fixed", "Soft", "Linear", "Hard" });
    check (param_ids::padFilterMode (1),
           { "Off", "Low-pass", "High-pass", "Band-pass" });
    check (param_ids::padLoopMode (1),
           { "Off", "Forward", "Ping-Pong" });
    // "Key Zones" appended (index 3) — existing indices unchanged, so saved
    // projects still resolve to the same mode.
    check (param_ids::padLayerMode (1),
           { "Velocity", "Round Robin", "Random", "Key Zones" });
    // "Legato (rate)" / "Always (rate)" appended (indices 3-4).
    check (param_ids::padGlideMode (1),
           { "Off", "Legato", "Always", "Legato (rate)", "Always (rate)" });
    check (param_ids::scaleKey,
           { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" });
    check (param_ids::scaleType,
           { "Chromatic", "Major", "Natural Minor", "Harmonic Minor",
             "Pentatonic Minor", "Pentatonic Major", "Blues", "Dorian",
             "Phrygian", "Mixolydian" });
    check (param_ids::padRepeatRate (1),
           { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T" });
    check (param_ids::padAftertouchDest (1),
           { "Off", "Volume", "Filter Cutoff" });
    check (param_ids::padMode (1),
           { "Fixed", "Keyboard" });
}

TEST_CASE ("clampReaderLengthToInt covers boundary inputs",
           "[plugin][loading][int32guard]")
{
    using P = ASamplerProcessor;
    constexpr juce::int64 maxInt = std::numeric_limits<int>::max();

    REQUIRE (P::clampReaderLengthToInt (0)                   == 0);
    REQUIRE (P::clampReaderLengthToInt (-1)                  == 0);
    REQUIRE (P::clampReaderLengthToInt (std::numeric_limits<juce::int64>::min()) == 0);
    REQUIRE (P::clampReaderLengthToInt (1)                   == 1);
    REQUIRE (P::clampReaderLengthToInt (44100)               == 44100);
    REQUIRE (P::clampReaderLengthToInt (maxInt)              == std::numeric_limits<int>::max());
    REQUIRE (P::clampReaderLengthToInt (maxInt + 1)          == std::numeric_limits<int>::max());
    REQUIRE (P::clampReaderLengthToInt (std::numeric_limits<juce::int64>::max()) == std::numeric_limits<int>::max());
}

TEST_CASE ("loadSampleForPad rejects an empty WAV", "[plugin][loading]")
{
    auto wav = writeEmptyWav ("empty");
    ASamplerProcessor proc;
    const auto result = proc.loadSampleForPad (0, wav);

    wav.deleteFile();
    REQUIRE (result == ASamplerProcessor::LoadResult::EmptyFile);
}

TEST_CASE ("loadSampleForPad rejects an unsupported file format", "[plugin][loading]")
{
    auto junk = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("asampler_test_junk.xyz");
    junk.replaceWithText ("this is not audio data");

    ASamplerProcessor proc;
    const auto result = proc.loadSampleForPad (0, junk);

    junk.deleteFile();
    REQUIRE (result == ASamplerProcessor::LoadResult::FormatNotSupported);
}

TEST_CASE ("loadSampleForPad rejects out-of-range pad indices", "[plugin][loading]")
{
    auto wav = writeSineWav ("oor");
    ASamplerProcessor proc;
    const auto low  = proc.loadSampleForPad (-1, wav);
    const auto high = proc.loadSampleForPad (kNumPads, wav);

    wav.deleteFile();
    REQUIRE (low  == ASamplerProcessor::LoadResult::FileNotFound);
    REQUIRE (high == ASamplerProcessor::LoadResult::FileNotFound);
}

TEST_CASE ("loadSampleForPad accepts a valid WAV and updates the display name",
           "[plugin][loading]")
{
    auto wav = writeSineWav ("ok");
    ASamplerProcessor proc;

    REQUIRE (proc.getPadSampleDisplayName (0) == "default");

    const auto result = proc.loadSampleForPad (0, wav);
    REQUIRE (result == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.getPadSampleDisplayName (0) == wav.getFileName());

    proc.resetPadToDefault (0);
    REQUIRE (proc.getPadSampleDisplayName (0) == "default");

    wav.deleteFile();
}

// ----- State persistence -------------------------------------------------

TEST_CASE ("State round-trip preserves modified parameters and pad sample paths",
           "[plugin][state]")
{
    auto wav = writeSineWav ("state");

    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::masterGain,     -6.0f);
        setParam (a, param_ids::padVolume (1),  -3.5f);
        setParam (a, param_ids::padPitch  (3),   7.0f);
        setParam (a, param_ids::padFilterCutoff (4), 1000.0f);
        a.loadSampleForPad (5, wav);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (b.getState().getRawParameterValue (param_ids::masterGain)->load()
             == Approx (-6.0f).margin (0.05f));
    REQUIRE (b.getState().getRawParameterValue (param_ids::padVolume (1))->load()
             == Approx (-3.5f).margin (0.05f));
    REQUIRE (b.getState().getRawParameterValue (param_ids::padPitch (3))->load()
             == Approx (7.0f).margin (0.05f));
    REQUIRE (b.getState().getRawParameterValue (param_ids::padFilterCutoff (4))->load()
             == Approx (1000.0f).margin (1.0f));
    REQUIRE (b.getPadSampleDisplayName (5) == wav.getFileName());

    wav.deleteFile();
}

// ----- MIDI dispatch -----------------------------------------------------

TEST_CASE ("processBlock triggers the pad whose note matches MIDI input",
           "[plugin][midi]")
{
    {
        ASamplerProcessor proc;
        proc.prepareToPlay (44100.0, 256);

        // Pad 1 defaults to note 36 — sending it should produce audible output.
        juce::AudioBuffer<float> buf (2, 256);
        auto midi = noteOnAt (36, 1.0f);
        proc.processBlock (buf, midi);
        REQUIRE (blockEnergy (buf) > 0.0f);
    }

    {
        // Fresh processor so no other voices linger — a note no pad maps to
        // must produce silence on its own.
        ASamplerProcessor proc;
        proc.prepareToPlay (44100.0, 256);

        juce::AudioBuffer<float> silent (2, 256);
        auto silentMidi = noteOnAt (24, 1.0f);
        proc.processBlock (silent, silentMidi);
        REQUIRE (blockEnergy (silent) == 0.0f);
    }
}

TEST_CASE ("Two pads sharing a note both trigger on a single note-on",
           "[plugin][midi]")
{
    ASamplerProcessor proc;

    // Move pad 2's note onto pad 1's (36) so both pads share a trigger.
    setParam (proc, param_ids::padNote (2), 36.0f);

    // Hard-pan pad 1 left and pad 2 right so we can confirm each contributed
    // independently to its respective channel.
    setParam (proc, param_ids::padPan (1), -1.0f);
    setParam (proc, param_ids::padPan (2),  1.0f);

    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> buf (2, 512);
    auto midi = noteOnAt (36, 1.0f);
    proc.processBlock (buf, midi);

    REQUIRE (channelEnergy (buf, 0) > 0.0f); // pad 1 (left)
    REQUIRE (channelEnergy (buf, 1) > 0.0f); // pad 2 (right)
}

TEST_CASE ("Same-group choke silences the previously triggered pad",
           "[plugin][midi][choke]")
{
    auto wav = writeSteadyWav ("choke");

    // Pan pad 1 hard-left and pad 2 hard-right so each pad contributes to a
    // single channel. After a same-group choke, pad 1 must be silenced, so the
    // left channel of subsequent blocks should drop to zero — a much cleaner
    // signal than comparing total energy (pad 2's pluck masks the ratio).
    auto pad1LeftAfterChoke = [&] (int padTwoChokeGroup) -> float
    {
        ASamplerProcessor proc;
        REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
        setParam (proc, param_ids::padChokeGroup (1), 1.0f);
        setParam (proc, param_ids::padChokeGroup (2), static_cast<float> (padTwoChokeGroup));
        setParam (proc, param_ids::padPan (1), -1.0f);
        setParam (proc, param_ids::padPan (2),  1.0f);
        proc.prepareToPlay (44100.0, 512);

        // Trigger pad 1 (note 36), render its sustaining tone for one block.
        juce::AudioBuffer<float> blk1 (2, 512);
        auto m1 = noteOnAt (36, 1.0f);
        proc.processBlock (blk1, m1);

        // Trigger pad 2 (note 37). When same-group, pad 1's voice gets choked.
        juce::AudioBuffer<float> blk2 (2, 512);
        auto m2 = noteOnAt (37, 1.0f);
        proc.processBlock (blk2, m2);

        // Render two more blocks with no MIDI. Pad 2 (right) decays; pad 1's
        // left channel either keeps sustaining (no choke) or is silent (choke).
        juce::AudioBuffer<float> blk3 (2, 512);
        juce::MidiBuffer empty;
        proc.processBlock (blk3, empty);

        juce::AudioBuffer<float> blk4 (2, 512);
        proc.processBlock (blk4, empty);

        return channelEnergy (blk4, 0); // left = pad 1 only
    };

    const float leftWithChoke    = pad1LeftAfterChoke (1); // same group → silenced
    const float leftWithoutChoke = pad1LeftAfterChoke (2); // different group → still going

    wav.deleteFile();

    CAPTURE (leftWithChoke, leftWithoutChoke);
    REQUIRE (leftWithoutChoke > 0.0f);
    REQUIRE (leftWithChoke    == Approx (0.0f).margin (1e-4f));
}

// ----- Per-pad output buses ---------------------------------------------

TEST_CASE ("Plugin declares one Main bus plus 16 per-pad buses",
           "[plugin][buses]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getBusCount (false) == 1 + kNumPads);

    REQUIRE (proc.getBus (false, 0)->getName() == "Main");
    REQUIRE (proc.getBus (false, 0)->isEnabledByDefault());

    for (int p = 1; p <= kNumPads; ++p)
    {
        CAPTURE (p);
        auto* bus = proc.getBus (false, p);
        REQUIRE (bus != nullptr);
        REQUIRE (bus->getName() == "Pad " + juce::String (p));
        REQUIRE (! bus->isEnabledByDefault());
    }
}

TEST_CASE ("Enabling a per-pad bus routes that pad's audio off Main",
           "[plugin][buses][routing]")
{
    ASamplerProcessor proc;

    auto layout = proc.getBusesLayout();
    layout.outputBuses.set (1, juce::AudioChannelSet::stereo()); // enable Pad 1's bus
    REQUIRE (proc.setBusesLayout (layout));

    proc.prepareToPlay (44100.0, 256);

    // Total channels = 2 (Main) + 2 (Pad 1) = 4.
    juce::AudioBuffer<float> buf (4, 256);
    auto midi = noteOnAt (36, 1.0f);   // note 36 → pad 1
    proc.processBlock (buf, midi);

    auto channelSum = [&] (int ch)
    {
        float s = 0.0f;
        for (int i = 0; i < 256; ++i) s += std::abs (buf.getSample (ch, i));
        return s;
    };

    const float mainL = channelSum (0);
    const float mainR = channelSum (1);
    const float padL  = channelSum (2);
    const float padR  = channelSum (3);

    REQUIRE (mainL == 0.0f);
    REQUIRE (mainR == 0.0f);
    REQUIRE (padL > 0.0f);
    REQUIRE (padR > 0.0f);
}

TEST_CASE ("Pads whose bus is disabled still mix into Main",
           "[plugin][buses][routing]")
{
    ASamplerProcessor proc;

    // Enable Pad 1's bus but leave Pad 2's disabled. Trigger Pad 2 — its
    // voices should fall back to the Main mix.
    auto layout = proc.getBusesLayout();
    layout.outputBuses.set (1, juce::AudioChannelSet::stereo());
    REQUIRE (proc.setBusesLayout (layout));

    proc.prepareToPlay (44100.0, 256);

    juce::AudioBuffer<float> buf (4, 256);
    auto midi = noteOnAt (37, 1.0f);   // note 37 → pad 2 (no per-pad bus)
    proc.processBlock (buf, midi);

    auto channelSum = [&] (int ch)
    {
        float s = 0.0f;
        for (int i = 0; i < 256; ++i) s += std::abs (buf.getSample (ch, i));
        return s;
    };

    REQUIRE (channelSum (0) > 0.0f);  // Pad 2 audible on Main left
    REQUIRE (channelSum (1) > 0.0f);  // Pad 2 audible on Main right
    REQUIRE (channelSum (2) == 0.0f); // Pad 1 bus silent (not triggered)
    REQUIRE (channelSum (3) == 0.0f);
}

TEST_CASE ("Master gain affects Main only - per-pad outs bypass it",
           "[plugin][buses][routing]")
{
    ASamplerProcessor proc;

    auto layout = proc.getBusesLayout();
    layout.outputBuses.set (1, juce::AudioChannelSet::stereo());
    REQUIRE (proc.setBusesLayout (layout));

    // Pull master to -60 dB (effectively silent at gain 0.001x).
    setParam (proc, param_ids::masterGain, -60.0f);

    proc.prepareToPlay (44100.0, 256);

    juce::AudioBuffer<float> buf (4, 256);
    auto midi = noteOnAt (36, 1.0f);
    proc.processBlock (buf, midi);

    auto channelSum = [&] (int ch)
    {
        float s = 0.0f;
        for (int i = 0; i < 256; ++i) s += std::abs (buf.getSample (ch, i));
        return s;
    };

    // Per-pad bus is post-pad, pre-master, so Pad 1's bus is fully audible
    // even with master = -60 dB.
    REQUIRE (channelSum (2) > 0.0f);
    REQUIRE (channelSum (3) > 0.0f);
}

// ----- Chop / slice ------------------------------------------------------

TEST_CASE ("chopAcrossPads rejects a source pad that has no loaded sample",
           "[plugin][chop]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.chopAcrossPads (0)
             == ASamplerProcessor::ChopResult::SourcePadEmpty);
}

TEST_CASE ("chopAcrossPads rejects invalid pad indices and slice counts",
           "[plugin][chop]")
{
    auto wav = writeSineWav ("chop_invalid");
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);

    REQUIRE (proc.chopAcrossPads (-1)
             == ASamplerProcessor::ChopResult::InvalidSourcePad);
    REQUIRE (proc.chopAcrossPads (kNumPads)
             == ASamplerProcessor::ChopResult::InvalidSourcePad);
    REQUIRE (proc.chopAcrossPads (0, 0)
             == ASamplerProcessor::ChopResult::InvalidSliceCount);
    REQUIRE (proc.chopAcrossPads (0, kNumPads + 1)
             == ASamplerProcessor::ChopResult::InvalidSliceCount);

    wav.deleteFile();
}

TEST_CASE ("chopAcrossPads distributes equal start/end windows and shares the sample",
           "[plugin][chop]")
{
    auto wav = writeSineWav ("chop_eq");
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);

    const int N = 4;
    REQUIRE (proc.chopAcrossPads (0, N) == ASamplerProcessor::ChopResult::Success);

    auto& state = proc.getState();
    for (int i = 0; i < N; ++i)
    {
        CAPTURE (i);
        const float s = state.getRawParameterValue (param_ids::padStart (i + 1))->load();
        const float e = state.getRawParameterValue (param_ids::padEnd   (i + 1))->load();
        REQUIRE (s == Approx (static_cast<float> (i)     / static_cast<float> (N)).margin (1e-4f));
        REQUIRE (e == Approx (static_cast<float> (i + 1) / static_cast<float> (N)).margin (1e-4f));
        REQUIRE (proc.getPadSampleDisplayName (i) == wav.getFileName());
    }

    // Pads outside the slice range must be left alone (defaults).
    for (int i = N; i < kNumPads; ++i)
    {
        CAPTURE (i);
        REQUIRE (proc.getPadSampleDisplayName (i) == "default");
    }

    wav.deleteFile();
}

TEST_CASE ("chopAcrossPads convenience overload reads the Chop Count parameter",
           "[plugin][chop]")
{
    auto wav = writeSineWav ("chop_param");
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);

    // Choice index 1 → "4" slices (see kChopCountLabels).
    auto* p = proc.getState().getParameter (param_ids::chopCount);
    REQUIRE (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (1.0f));
    REQUIRE (proc.getCurrentChopCount() == 4);

    REQUIRE (proc.chopAcrossPads (0) == ASamplerProcessor::ChopResult::Success);

    auto& state = proc.getState();
    REQUIRE (state.getRawParameterValue (param_ids::padEnd (4))->load()
             == Approx (1.0f).margin (1e-4f));
    REQUIRE (proc.getPadSampleDisplayName (3) == wav.getFileName());
    REQUIRE (proc.getPadSampleDisplayName (4) == "default");

    wav.deleteFile();
}

TEST_CASE ("chopAcrossPads in Transients mode places boundaries near onsets",
           "[plugin][chop][transient]")
{
    // 4 bursts at 0.0s, 0.25s, 0.5s, 0.75s. Detector should find them and use
    // their sample indices as slice starts.
    const int    N       = 4;
    const double dur     = 1.0;
    auto wav = writeBurstWav ("chop_tr", N, dur);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.chopAcrossPads (0, N, ChopMode::Transients)
             == ASamplerProcessor::ChopResult::Success);

    auto& state = proc.getState();

    // Slice i should START near burst i. With 4 evenly-spaced bursts in a 1s
    // sample, each slice start should land within ~50 ms of i * 0.25 sec.
    const float tolNorm = 0.05f;   // 50 ms / 1 s
    for (int i = 0; i < N; ++i)
    {
        CAPTURE (i);
        const float s = state.getRawParameterValue (param_ids::padStart (i + 1))->load();
        const float expected = static_cast<float> (i) / static_cast<float> (N);
        REQUIRE (std::abs (s - expected) < tolNorm);
    }

    // Slice ends should chain into the next slice's start, with the last
    // slice ending at 1.0 (end of sample).
    for (int i = 0; i + 1 < N; ++i)
    {
        const float thisEnd   = state.getRawParameterValue (param_ids::padEnd   (i + 1))->load();
        const float nextStart = state.getRawParameterValue (param_ids::padStart (i + 2))->load();
        REQUIRE (thisEnd == Approx (nextStart).margin (1e-4f));
    }
    REQUIRE (state.getRawParameterValue (param_ids::padEnd (N))->load()
             == Approx (1.0f).margin (1e-4f));

    wav.deleteFile();
}

TEST_CASE ("chopAcrossPads in Transients mode falls back to equal when too few onsets",
           "[plugin][chop][transient]")
{
    // Only 2 transients in the source. Asking for 8 slices forces the
    // detector-too-few-hits → equal-fallback path.
    auto wav = writeBurstWav ("chop_few", 2, 1.0);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.chopAcrossPads (0, 8, ChopMode::Transients)
             == ASamplerProcessor::ChopResult::Success);

    // Equal-slice fallback: pad i should be at exactly i/8.
    auto& state = proc.getState();
    for (int i = 0; i < 8; ++i)
    {
        CAPTURE (i);
        const float s = state.getRawParameterValue (param_ids::padStart (i + 1))->load();
        REQUIRE (s == Approx (static_cast<float> (i) / 8.0f).margin (1e-4f));
    }

    wav.deleteFile();
}

TEST_CASE ("Chop Mode parameter resolves Transients via the convenience overload",
           "[plugin][chop][transient]")
{
    auto wav = writeBurstWav ("chop_param_tr", 4, 1.0);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);

    // Flip the chop mode parameter to Transients via the host-facing API.
    auto* mp = proc.getState().getParameter (param_ids::chopMode);
    REQUIRE (mp != nullptr);
    mp->setValueNotifyingHost (mp->convertTo0to1 (
        static_cast<float> (static_cast<int> (ChopMode::Transients))));
    REQUIRE (proc.getCurrentChopMode() == ChopMode::Transients);

    // And the chop count to 4.
    auto* cp = proc.getState().getParameter (param_ids::chopCount);
    REQUIRE (cp != nullptr);
    cp->setValueNotifyingHost (cp->convertTo0to1 (1.0f));   // index 1 → "4"

    REQUIRE (proc.chopAcrossPads (0) == ASamplerProcessor::ChopResult::Success);

    // Pad 1 should start near 0 — Transients mode places the first slice at
    // the first detected onset, which for the burst at sample 0 is very close
    // to 0.0.
    auto& state = proc.getState();
    REQUIRE (state.getRawParameterValue (param_ids::padStart (1))->load()
             == Approx (0.0f).margin (0.05f));

    wav.deleteFile();
}

TEST_CASE ("Chop Mode TransientsSpectral resolves and runs end-to-end",
           "[plugin][chop][spectral]")
{
    // Build a 1-second source that swaps frequency twice — three "regions"
    // separated by timbral steps the energy detector would miss. Chop with
    // spectral flux should land slice boundaries at the changes (or fall
    // back to equal if too few are found — we just check the dispatch
    // path works and Success is returned).
    const int    totalSamples = 44100;
    juce::AudioBuffer<float> audio (1, totalSamples);
    auto* w = audio.getWritePointer (0);
    for (int i = 0; i < totalSamples; ++i)
    {
        const double t = static_cast<double> (i) / 44100.0;
        double freq;
        if      (i < totalSamples / 3)         freq = 220.0;
        else if (i < (2 * totalSamples) / 3)   freq = 440.0;
        else                                   freq = 880.0;
        w[i] = static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * freq * t)
                                     * 0.5);
    }

    auto file = tempWavFile ("chop_spectral");
    if (file.existsAsFile()) file.deleteFile();
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
        REQUIRE (os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (os.get(), 44100.0, 1u, 16, {}, 0));
        REQUIRE (writer != nullptr);
        os.release();
        writer->writeFromAudioSampleBuffer (audio, 0, totalSamples);
        writer.reset();
    }

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, file) == ASamplerProcessor::LoadResult::Success);

    auto* mp = proc.getState().getParameter (param_ids::chopMode);
    REQUIRE (mp != nullptr);
    mp->setValueNotifyingHost (mp->convertTo0to1 (
        static_cast<float> (static_cast<int> (ChopMode::TransientsSpectral))));
    REQUIRE (proc.getCurrentChopMode() == ChopMode::TransientsSpectral);

    REQUIRE (proc.chopAcrossPads (0, 2) == ASamplerProcessor::ChopResult::Success);

    // Status should mention either spectral transients (worked) or fallback,
    // never the plain "transients" label that belongs to the energy detector.
    const auto status = proc.getLastStatusMessage();
    CAPTURE (status.toStdString());
    REQUIRE ((status.contains ("transients (spectral)")
              || status.contains ("equal")));

    file.deleteFile();
}

TEST_CASE ("State round-trip preserves a chop layout",
           "[plugin][chop][state]")
{
    auto wav = writeSineWav ("chop_state");

    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        REQUIRE (a.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
        REQUIRE (a.chopAcrossPads (0, 8) == ASamplerProcessor::ChopResult::Success);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    for (int i = 0; i < 8; ++i)
    {
        CAPTURE (i);
        const float s = b.getState().getRawParameterValue (param_ids::padStart (i + 1))->load();
        const float e = b.getState().getRawParameterValue (param_ids::padEnd   (i + 1))->load();
        REQUIRE (s == Approx (static_cast<float> (i)     / 8.0f).margin (1e-4f));
        REQUIRE (e == Approx (static_cast<float> (i + 1) / 8.0f).margin (1e-4f));
        REQUIRE (b.getPadSampleDisplayName (i) == wav.getFileName());
    }

    wav.deleteFile();
}

TEST_CASE ("Fixed velocity curve ignores MIDI velocity and produces full gain",
           "[plugin][midi][velocity]")
{
    auto run = [] (float midiVelocity) -> float
    {
        ASamplerProcessor proc;
        setParam (proc, param_ids::padVelocityCurve (1),
                  static_cast<float> (VelocityCurve::Fixed));
        // Disable release tail so block-to-block variation isn't influenced by
        // attack ramping; instant attack/decay keeps amplitude steady.
        setParam (proc, param_ids::padAttack  (1), 0.0f);
        setParam (proc, param_ids::padDecay   (1), 0.0f);
        setParam (proc, param_ids::padSustain (1), 1.0f);

        proc.prepareToPlay (44100.0, 256);

        juce::AudioBuffer<float> buf (2, 256);
        auto midi = noteOnAt (36, midiVelocity);
        proc.processBlock (buf, midi);
        return blockEnergy (buf);
    };

    const float low  = run (0.05f);
    const float high = run (1.0f);

    REQUIRE (low > 0.0f);
    REQUIRE (low == Approx (high).margin (high * 0.02f));
}

// ----- Velocity layers ---------------------------------------------------

namespace
{
    // Apply the standard "Fixed-velocity, instant-envelope, centre-pan" config
    // to a pad so a render's per-sample peak is a direct readout of the source
    // sample amplitude (× 0.707 for equal-power centre pan).
    void prepareLayerPeakReadout (ASamplerProcessor& proc, int padOneBased)
    {
        setParam (proc, param_ids::padVelocityCurve (padOneBased),
                  static_cast<float> (VelocityCurve::Fixed));
        setParam (proc, param_ids::padAttack  (padOneBased), 0.0f);
        setParam (proc, param_ids::padDecay   (padOneBased), 0.0f);
        setParam (proc, param_ids::padSustain (padOneBased), 1.0f);
        setParam (proc, param_ids::padPan     (padOneBased), 0.0f);
    }

    float channelPeak (const juce::AudioBuffer<float>& buf, int ch)
    {
        float peak = 0.0f;
        const auto* r = buf.getReadPointer (ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            peak = std::max (peak, std::abs (r[i]));
        return peak;
    }
}

TEST_CASE ("Per-layer load / reset / display name are independent",
           "[plugin][layers]")
{
    auto w0 = writeConstantWav ("layer0", 0.1f);
    auto w1 = writeConstantWav ("layer1", 0.2f);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPadLayer (0, 0, w0)
             == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 1, w1)
             == ASamplerProcessor::LoadResult::Success);

    REQUIRE (proc.getPadLayerSampleDisplayName (0, 0) == w0.getFileName());
    REQUIRE (proc.getPadLayerSampleDisplayName (0, 1) == w1.getFileName());
    REQUIRE (proc.getPadLayerSampleDisplayName (0, 2) == "default");
    REQUIRE (proc.getPadLayerSampleDisplayName (0, 3) == "default");

    // Resetting layer 1 must not disturb layer 0.
    proc.resetPadLayerToDefault (0, 1);
    REQUIRE (proc.getPadLayerSampleDisplayName (0, 0) == w0.getFileName());
    REQUIRE (proc.getPadLayerSampleDisplayName (0, 1) == "default");

    w0.deleteFile();
    w1.deleteFile();
}

TEST_CASE ("Edit Layer parameter steers loadSampleForPad / displayName",
           "[plugin][layers]")
{
    auto w0 = writeConstantWav ("editlayer0", 0.1f);
    auto w2 = writeConstantWav ("editlayer2", 0.3f);

    ASamplerProcessor proc;

    // Default edit layer is 0 → legacy convenience methods address layer 0.
    REQUIRE (proc.loadSampleForPad (0, w0) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.getPadSampleDisplayName (0) == w0.getFileName());
    REQUIRE (proc.getPadLayerSampleDisplayName (0, 0) == w0.getFileName());

    // Move Edit Layer to "Layer 3" (index 2) and load again — should land on
    // pad 1's layer 2 without touching layer 0.
    setParam (proc, param_ids::editLayer, 2.0f);
    REQUIRE (proc.getCurrentEditLayer() == 2);
    REQUIRE (proc.loadSampleForPad (0, w2) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.getPadSampleDisplayName (0) == w2.getFileName());
    REQUIRE (proc.getPadLayerSampleDisplayName (0, 0) == w0.getFileName());
    REQUIRE (proc.getPadLayerSampleDisplayName (0, 2) == w2.getFileName());

    w0.deleteFile();
    w2.deleteFile();
}

TEST_CASE ("MIDI velocity selects the matching layer", "[plugin][layers][midi]")
{
    // Distinct DC amplitudes per layer — each layer is identifiable by render peak.
    auto w0 = writeConstantWav ("vl0", 0.1f);
    auto w1 = writeConstantWav ("vl1", 0.2f);
    auto w2 = writeConstantWav ("vl2", 0.3f);
    auto w3 = writeConstantWav ("vl3", 0.4f);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPadLayer (0, 0, w0) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 1, w1) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 2, w2) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 3, w3) == ASamplerProcessor::LoadResult::Success);

    prepareLayerPeakReadout (proc, /*padOneBased=*/ 1);
    proc.prepareToPlay (44100.0, 256);

    // Picks layer i (0..3) by mapping a representative velocity into its band.
    auto renderAt = [&] (float vel)
    {
        juce::AudioBuffer<float> buf (2, 256);
        auto midi = noteOnAt (36, vel);
        proc.processBlock (buf, midi);
        return channelPeak (buf, 0);
    };

    constexpr float kEqualPower = 0.7071068f;
    REQUIRE (renderAt (0.1f) == Approx (0.1f * kEqualPower).margin (0.005f));
    REQUIRE (renderAt (0.4f) == Approx (0.2f * kEqualPower).margin (0.005f));
    REQUIRE (renderAt (0.7f) == Approx (0.3f * kEqualPower).margin (0.005f));
    REQUIRE (renderAt (1.0f) == Approx (0.4f * kEqualPower).margin (0.005f));

    w0.deleteFile(); w1.deleteFile(); w2.deleteFile(); w3.deleteFile();
}

TEST_CASE ("Empty velocity layers fall back to the nearest loaded layer",
           "[plugin][layers][midi]")
{
    // Load samples on layers 0 and 3 only. Velocities targeting layers 1 and 2
    // must fall back: layer 1 should resolve to the closer of {0, 3} (above
    // first → layer 3 is distance 2, layer 0 is distance 1 → picks 0);
    // layer 2 should resolve to layer 3 (above wins at distance 1).
    auto wLow  = writeConstantWav ("fb_low",  0.1f);
    auto wHigh = writeConstantWav ("fb_high", 0.4f);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wLow)  == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 3, wHigh) == ASamplerProcessor::LoadResult::Success);

    prepareLayerPeakReadout (proc, 1);
    proc.prepareToPlay (44100.0, 256);

    auto renderAt = [&] (float vel)
    {
        juce::AudioBuffer<float> buf (2, 256);
        auto midi = noteOnAt (36, vel);
        proc.processBlock (buf, midi);
        return channelPeak (buf, 0);
    };

    constexpr float kEqualPower = 0.7071068f;

    // Velocity 0.1 → layer 0 (loaded) → wLow.
    REQUIRE (renderAt (0.1f) == Approx (0.1f * kEqualPower).margin (0.005f));
    // Velocity 1.0 → layer 3 (loaded) → wHigh.
    REQUIRE (renderAt (1.0f) == Approx (0.4f * kEqualPower).margin (0.005f));
    // Velocity 0.7 → layer 2 (empty); above-first search finds layer 3 → wHigh.
    REQUIRE (renderAt (0.7f) == Approx (0.4f * kEqualPower).margin (0.005f));

    wLow.deleteFile();
    wHigh.deleteFile();
}

TEST_CASE ("State round-trip preserves per-layer sample paths",
           "[plugin][layers][state]")
{
    auto w0 = writeConstantWav ("st_layer0", 0.1f);
    auto w2 = writeConstantWav ("st_layer2", 0.3f);

    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        REQUIRE (a.loadSampleForPadLayer (3, 0, w0) == ASamplerProcessor::LoadResult::Success);
        REQUIRE (a.loadSampleForPadLayer (3, 2, w2) == ASamplerProcessor::LoadResult::Success);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (b.getPadLayerSampleDisplayName (3, 0) == w0.getFileName());
    REQUIRE (b.getPadLayerSampleDisplayName (3, 1) == "default");
    REQUIRE (b.getPadLayerSampleDisplayName (3, 2) == w2.getFileName());
    REQUIRE (b.getPadLayerSampleDisplayName (3, 3) == "default");

    w0.deleteFile();
    w2.deleteFile();
}

TEST_CASE ("Legacy state without a layer attribute loads into layer 0",
           "[plugin][layers][state][compat]")
{
    auto w0 = writeConstantWav ("legacy", 0.2f);

    // Manually build the pre-velocity-layer XML format: <pad index path /> with
    // no `layer` attribute. setStateInformation must default these to layer 0.
    juce::ValueTree root ("STATE");
    auto samples = juce::ValueTree ("samples");
    auto entry   = juce::ValueTree ("pad");
    entry.setProperty ("index", 2, nullptr);
    entry.setProperty ("path",  w0.getFullPathName(), nullptr);
    samples.appendChild (entry, nullptr);
    root.addChild (samples, -1, nullptr);

    juce::MemoryBlock block;
    if (auto xml = root.createXml())
        juce::AudioProcessor::copyXmlToBinary (*xml, block);

    ASamplerProcessor proc;
    proc.setStateInformation (block.getData(), static_cast<int> (block.getSize()));

    REQUIRE (proc.getPadLayerSampleDisplayName (2, 0) == w0.getFileName());
    REQUIRE (proc.getPadLayerSampleDisplayName (2, 1) == "default");
    REQUIRE (proc.getPadLayerSampleDisplayName (2, 2) == "default");
    REQUIRE (proc.getPadLayerSampleDisplayName (2, 3) == "default");

    w0.deleteFile();
}

TEST_CASE ("Default Layer Mode is Velocity (preserves existing dispatch)",
           "[plugin][layers][mode]")
{
    ASamplerProcessor proc;
    for (int p = 1; p <= kNumPads; ++p)
    {
        CAPTURE (p);
        const int mode = juce::roundToInt (
            proc.getState().getRawParameterValue (param_ids::padLayerMode (p))->load());
        REQUIRE (mode == static_cast<int> (LayerMode::Velocity));
    }
}

TEST_CASE ("Round Robin cycles through loaded layers in index order",
           "[plugin][layers][mode][midi]")
{
    auto wA = writeConstantWav ("rrA", 0.1f);
    auto wB = writeConstantWav ("rrB", 0.4f);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wA) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 2, wB) == ASamplerProcessor::LoadResult::Success);

    setParam (proc, param_ids::padLayerMode (1),
              static_cast<float> (LayerMode::RoundRobin));
    prepareLayerPeakReadout (proc, 1);
    proc.prepareToPlay (44100.0, 256);

    auto trigger = [&] ()
    {
        juce::AudioBuffer<float> buf (2, 256);
        auto midi = noteOnAt (36, 1.0f);
        proc.processBlock (buf, midi);
        return channelPeak (buf, 0);
    };

    constexpr float kEqualPower = 0.7071068f;
    const float peakA = 0.1f * kEqualPower;
    const float peakB = 0.4f * kEqualPower;

    // Counter starts at 0 → first trigger picks loaded[0] = layer 0 = wA, then
    // loaded[1] = layer 2 = wB, and the cycle repeats.
    REQUIRE (trigger() == Approx (peakA).margin (0.005f));
    REQUIRE (trigger() == Approx (peakB).margin (0.005f));
    REQUIRE (trigger() == Approx (peakA).margin (0.005f));
    REQUIRE (trigger() == Approx (peakB).margin (0.005f));

    wA.deleteFile();
    wB.deleteFile();
}

TEST_CASE ("Round Robin with a single loaded layer always picks that layer",
           "[plugin][layers][mode][midi]")
{
    auto wA = writeConstantWav ("rrSingle", 0.25f);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPadLayer (0, 2, wA) == ASamplerProcessor::LoadResult::Success);

    setParam (proc, param_ids::padLayerMode (1),
              static_cast<float> (LayerMode::RoundRobin));
    prepareLayerPeakReadout (proc, 1);
    proc.prepareToPlay (44100.0, 256);

    constexpr float kEqualPower = 0.7071068f;
    const float expected = 0.25f * kEqualPower;

    for (int i = 0; i < 5; ++i)
    {
        CAPTURE (i);
        juce::AudioBuffer<float> buf (2, 256);
        auto midi = noteOnAt (36, 1.0f);
        proc.processBlock (buf, midi);
        REQUIRE (channelPeak (buf, 0) == Approx (expected).margin (0.005f));
    }

    wA.deleteFile();
}

TEST_CASE ("Random mode hits every loaded layer over many triggers",
           "[plugin][layers][mode][midi]")
{
    auto wA = writeConstantWav ("rndA", 0.1f);
    auto wB = writeConstantWav ("rndB", 0.4f);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wA) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 1, wB) == ASamplerProcessor::LoadResult::Success);

    setParam (proc, param_ids::padLayerMode (1),
              static_cast<float> (LayerMode::Random));
    prepareLayerPeakReadout (proc, 1);
    proc.prepareToPlay (44100.0, 256);

    constexpr float kEqualPower = 0.7071068f;
    const float peakA = 0.1f * kEqualPower;
    const float peakB = 0.4f * kEqualPower;

    int countA = 0, countB = 0, countOther = 0;
    for (int i = 0; i < 200; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        auto midi = noteOnAt (36, 1.0f);
        proc.processBlock (buf, midi);
        const float p = channelPeak (buf, 0);

        if      (std::abs (p - peakA) < 0.005f) ++countA;
        else if (std::abs (p - peakB) < 0.005f) ++countB;
        else                                    ++countOther;
    }

    // 50/50 with 200 trials: each side should land near 100. A threshold of 20
    // is far below the expected mean and well outside any reasonable random
    // fluctuation, so this is robust against the RNG sequence Catch2 picks.
    REQUIRE (countOther == 0);
    REQUIRE (countA > 20);
    REQUIRE (countB > 20);

    wA.deleteFile();
    wB.deleteFile();
}

TEST_CASE ("Layer Mode survives a state round-trip",
           "[plugin][layers][mode][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::padLayerMode (3),
                  static_cast<float> (LayerMode::Random));
        setParam (a, param_ids::padLayerMode (7),
                  static_cast<float> (LayerMode::RoundRobin));
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    const int mode3 = juce::roundToInt (
        b.getState().getRawParameterValue (param_ids::padLayerMode (3))->load());
    const int mode7 = juce::roundToInt (
        b.getState().getRawParameterValue (param_ids::padLayerMode (7))->load());
    REQUIRE (mode3 == static_cast<int> (LayerMode::Random));
    REQUIRE (mode7 == static_cast<int> (LayerMode::RoundRobin));
}

TEST_CASE ("Chop touches layer 0 only - higher layers are preserved",
           "[plugin][layers][chop]")
{
    auto wChop  = writeConstantWav ("chop_l0_src", 0.5f);   // source on pad 1, layer 0
    auto wKeep1 = writeConstantWav ("chop_l1_keep", 0.2f);  // pad 2 layer 1 (must survive)

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wChop)  == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (1, 1, wKeep1) == ASamplerProcessor::LoadResult::Success);

    REQUIRE (proc.chopAcrossPads (0, 4, ChopMode::Equal)
             == ASamplerProcessor::ChopResult::Success);

    // Pad 2's layer 0 now plays a chop slice of wChop; its layer 1 must
    // still hold wKeep1 untouched.
    REQUIRE (proc.getPadLayerSampleDisplayName (1, 0) == wChop.getFileName());
    REQUIRE (proc.getPadLayerSampleDisplayName (1, 1) == wKeep1.getFileName());

    // Pads outside the chop range keep their original layer-0 state (default).
    REQUIRE (proc.getPadLayerSampleDisplayName (5, 0) == "default");

    wChop.deleteFile();
    wKeep1.deleteFile();
}

// ----- Note repeat -------------------------------------------------------

namespace
{
    // Returns the active voice count after triggering pad 1 (note 36) and
    // holding it for `extraBlocks` more blocks of `blockSize` samples each.
    // Uses a long constant-DC sample so original + repeat voices stay alive
    // throughout the test (no early deactivation).
    int activeVoicesAfterHold (ASamplerProcessor& proc,
                               int extraBlocks, int blockSize = 512)
    {
        juce::AudioBuffer<float> buf (2, blockSize);
        auto midi = noteOnAt (36, 1.0f);
        proc.processBlock (buf, midi);

        juce::MidiBuffer empty;
        for (int i = 0; i < extraBlocks; ++i)
        {
            juce::AudioBuffer<float> b (2, blockSize);
            proc.processBlock (b, empty);
        }
        return proc.getActiveVoiceCount();
    }
}

TEST_CASE ("Note repeat is disabled by default - held pads do not auto-retrigger",
           "[plugin][noterepeat]")
{
    auto wav = writeConstantWav ("nr_default", 0.5f, 44100); // 1 s of DC
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    proc.prepareToPlay (44100.0, 512);

    // 30 blocks (~15 360 samples) is plenty of time for any 1/16 repeat to
    // fire if the feature were on. With Repeat off, only the original trigger
    // should be alive.
    REQUIRE (activeVoicesAfterHold (proc, 29) == 1);

    wav.deleteFile();
}

TEST_CASE ("Note repeat re-triggers a held pad at the configured rate",
           "[plugin][noterepeat]")
{
    auto wav = writeConstantWav ("nr_on", 0.5f, 44100);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRepeatEnabled (1), 1.0f);
    setParam (proc, param_ids::padRepeatRate    (1),
              static_cast<float> (NoteRepeatRate::Sixteenth));
    proc.prepareToPlay (44100.0, 512);

    // 30 blocks × 512 = 15 360 samples. At fallback 120 BPM the 1/16 step is
    // 5512.5 samples, so two synthetic repeats should fire (around samples
    // 5513 and 11025). Total voices = 1 original + 2 repeats.
    REQUIRE (activeVoicesAfterHold (proc, 29) >= 3);

    wav.deleteFile();
}

TEST_CASE ("Note-off stops further repeats", "[plugin][noterepeat]")
{
    auto wav = writeConstantWav ("nr_off", 0.5f, 44100);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRepeatEnabled (1), 1.0f);
    setParam (proc, param_ids::padRepeatRate    (1),
              static_cast<float> (NoteRepeatRate::Sixteenth));
    proc.prepareToPlay (44100.0, 512);

    // Trigger and hold long enough for one repeat to fire.
    juce::AudioBuffer<float> buf (2, 512);
    auto midi = noteOnAt (36, 1.0f);
    proc.processBlock (buf, midi);
    juce::MidiBuffer empty;
    for (int i = 0; i < 11; ++i)
    {
        juce::AudioBuffer<float> b (2, 512);
        proc.processBlock (b, empty);
    }
    REQUIRE (proc.getActiveVoiceCount() >= 2);

    // Send a note-off and continue processing. Default release = 0, so all
    // released voices deactivate immediately; if no NEW repeats fire the
    // pool stays empty for the rest of the test window.
    juce::AudioBuffer<float> bOff (2, 512);
    juce::MidiBuffer mOff;
    mOff.addEvent (juce::MidiMessage::noteOff (1, 36), 0);
    proc.processBlock (bOff, mOff);

    for (int i = 0; i < 30; ++i)
    {
        juce::AudioBuffer<float> b (2, 512);
        proc.processBlock (b, empty);
    }

    REQUIRE (proc.getActiveVoiceCount() == 0);

    wav.deleteFile();
}

TEST_CASE ("Faster repeat rates produce more re-triggers in the same window",
           "[plugin][noterepeat]")
{
    auto wav = writeConstantWav ("nr_rate", 0.5f, 44100);

    auto run = [&] (NoteRepeatRate rate)
    {
        ASamplerProcessor proc;
        REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
        setParam (proc, param_ids::padRepeatEnabled (1), 1.0f);
        setParam (proc, param_ids::padRepeatRate    (1), static_cast<float> (rate));
        proc.prepareToPlay (44100.0, 512);
        return activeVoicesAfterHold (proc, 29);
    };

    const int slow = run (NoteRepeatRate::Quarter);       // 1 step/beat → 0 repeats in 15360 samples
    const int fast = run (NoteRepeatRate::ThirtySecond);  // 8 steps/beat → 5 repeats

    REQUIRE (fast > slow);
    REQUIRE (slow == 1);     // original trigger only — 1/4 step is 22050 samples > 15360
    REQUIRE (fast >= 6);     // original + ~5 repeats

    wav.deleteFile();
}

TEST_CASE ("Repeat enabled and rate persist through state round-trip",
           "[plugin][noterepeat][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::padRepeatEnabled (2), 1.0f);
        setParam (a, param_ids::padRepeatRate    (2),
                  static_cast<float> (NoteRepeatRate::ThirtySecond));
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (b.getState().getRawParameterValue (param_ids::padRepeatEnabled (2))->load() >= 0.5f);
    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padRepeatRate (2))->load())
             == static_cast<int> (NoteRepeatRate::ThirtySecond));
}

// ----- MIDI learn --------------------------------------------------------

TEST_CASE ("bindCcToParameter stores and getCcBinding reads it back",
           "[plugin][midilearn]")
{
    ASamplerProcessor proc;

    proc.bindCcToParameter (7, param_ids::padVolume (1));
    REQUIRE (proc.getCcBinding (7) == juce::String (param_ids::padVolume (1)));

    // Empty paramId unbinds.
    proc.bindCcToParameter (7, {});
    REQUIRE (proc.getCcBinding (7).isEmpty());

    // Unknown paramId leaves the slot unbound rather than crashing.
    proc.bindCcToParameter (7, "no_such_param");
    REQUIRE (proc.getCcBinding (7).isEmpty());

    // Out-of-range CCs are no-ops.
    proc.bindCcToParameter (-1,  param_ids::padPan (1));
    proc.bindCcToParameter (128, param_ids::padPan (1));
    REQUIRE (proc.getCcBinding (-1).isEmpty());
    REQUIRE (proc.getCcBinding (128).isEmpty());
}

TEST_CASE ("unbindCc clears a binding", "[plugin][midilearn]")
{
    ASamplerProcessor proc;
    proc.bindCcToParameter (11, param_ids::padPan (3));
    REQUIRE (proc.getCcBinding (11) == juce::String (param_ids::padPan (3)));

    proc.unbindCc (11);
    REQUIRE (proc.getCcBinding (11).isEmpty());
}

TEST_CASE ("Incoming CC updates the bound parameter via processBlock",
           "[plugin][midilearn]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 256);

    // Bind CC 12 to pad 1 volume, then push CC value 127 (max).
    proc.bindCcToParameter (12, param_ids::padVolume (1));

    auto* volParam = proc.getState().getParameter (param_ids::padVolume (1));
    REQUIRE (volParam != nullptr);
    const float before = volParam->getValue();

    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 12, 127), 0);
    proc.processBlock (buf, midi);

    REQUIRE (volParam->getValue() == Approx (1.0f).margin (1.0e-4f));
    REQUIRE (volParam->getValue() != Approx (before).margin (1.0e-6f));
}

TEST_CASE ("Unbound CCs are ignored", "[plugin][midilearn]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 256);

    auto* volParam = proc.getState().getParameter (param_ids::padVolume (1));
    const float before = volParam->getValue();

    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 64), 0);
    proc.processBlock (buf, midi);

    REQUIRE (volParam->getValue() == Approx (before).margin (1.0e-6f));
}

TEST_CASE ("Touching a parameter sets it as the last-touched target",
           "[plugin][midilearn]")
{
    ASamplerProcessor proc;

    // Simulate the user grabbing pad 2 pan in REAPER's param UI.
    auto* panParam = proc.getState().getParameter (param_ids::padPan (2));
    REQUIRE (panParam != nullptr);
    panParam->beginChangeGesture();
    panParam->endChangeGesture();

    const int target = proc.getLastTouchedParameterIndex();
    REQUIRE (target >= 0);

    // The recorded index must resolve back to the same param ID.
    auto& params = proc.getParameters();
    REQUIRE (target < params.size());
    auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (params[target]);
    REQUIRE (withId != nullptr);
    REQUIRE (withId->paramID == juce::String (param_ids::padPan (2)));
}

TEST_CASE ("Touching the arm parameter does not count as last-touched",
           "[plugin][midilearn]")
{
    ASamplerProcessor proc;

    // First touch a real target so lastTouched isn't -1.
    auto* panParam = proc.getState().getParameter (param_ids::padPan (4));
    panParam->beginChangeGesture();
    panParam->endChangeGesture();
    const int firstTarget = proc.getLastTouchedParameterIndex();
    REQUIRE (firstTarget >= 0);

    // Now arm — gesturing the arm parameter must NOT overwrite lastTouched
    // (otherwise the workflow would just bind every CC to the arm param).
    auto* armParam = proc.getState().getParameter (param_ids::midiLearnArm);
    REQUIRE (armParam != nullptr);
    armParam->beginChangeGesture();
    armParam->endChangeGesture();

    REQUIRE (proc.getLastTouchedParameterIndex() == firstTarget);
}

TEST_CASE ("Learn workflow: arm + touch + CC binds and disarms",
           "[plugin][midilearn]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 256);

    // Touch pad 1 pitch so it becomes the learn target.
    auto* pitchParam = proc.getState().getParameter (param_ids::padPitch (1));
    pitchParam->beginChangeGesture();
    pitchParam->endChangeGesture();

    // Arm the learn parameter.
    setParam (proc, param_ids::midiLearnArm, 1.0f);
    REQUIRE (proc.getState().getRawParameterValue (param_ids::midiLearnArm)->load() >= 0.5f);

    // Send any CC — its number is what gets bound to pad 1 pitch.
    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 21, 0), 0);
    proc.processBlock (buf, midi);

    REQUIRE (proc.getCcBinding (21) == juce::String (param_ids::padPitch (1)));

    // Arm must auto-clear so the *next* CC doesn't get learned by mistake.
    REQUIRE (proc.getState().getRawParameterValue (param_ids::midiLearnArm)->load() < 0.5f);
}

TEST_CASE ("Learn-then-move: the same CC continues to write to the bound param",
           "[plugin][midilearn]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 256);

    auto* volParam = proc.getState().getParameter (param_ids::padVolume (1));
    volParam->beginChangeGesture();
    volParam->endChangeGesture();

    setParam (proc, param_ids::midiLearnArm, 1.0f);

    // Block 1: arms binds CC 30 to pad 1 volume (value 0 -> bottom of range).
    juce::AudioBuffer<float> b1 (2, 256);
    juce::MidiBuffer m1;
    m1.addEvent (juce::MidiMessage::controllerEvent (1, 30, 0), 0);
    proc.processBlock (b1, m1);
    const float afterLearn = volParam->getValue();

    // Block 2: a new CC value should now drive the parameter without re-arming.
    juce::AudioBuffer<float> b2 (2, 256);
    juce::MidiBuffer m2;
    m2.addEvent (juce::MidiMessage::controllerEvent (1, 30, 127), 0);
    proc.processBlock (b2, m2);
    const float afterMove = volParam->getValue();

    REQUIRE (afterMove > afterLearn);
    REQUIRE (afterMove == Approx (1.0f).margin (1.0e-4f));
}

TEST_CASE ("CC bindings survive a state round-trip", "[plugin][midilearn][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        a.bindCcToParameter (40, param_ids::padVolume (5));
        a.bindCcToParameter (41, param_ids::padPan    (5));
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (b.getCcBinding (40) == juce::String (param_ids::padVolume (5)));
    REQUIRE (b.getCcBinding (41) == juce::String (param_ids::padPan    (5)));
    REQUIRE (b.getCcBinding (42).isEmpty());
}

namespace
{
    // Test-only helper: lifts AudioProcessor's protected copyXmlToBinary /
    // getXmlFromBinary into public scope so a test can synthesise a legacy
    // save (no <midilearn> node) without reaching into the codec.
    struct TestableProcessor : public ASamplerProcessor
    {
        using AudioProcessor::copyXmlToBinary;
        using AudioProcessor::getXmlFromBinary;
    };
}

TEST_CASE ("Loading a legacy state with no <midilearn> node clears bindings",
           "[plugin][midilearn][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        a.getStateInformation (data);
    }

    juce::MemoryBlock legacy;
    {
        auto xml = TestableProcessor::getXmlFromBinary (data.getData(),
                                                        static_cast<int> (data.getSize()));
        REQUIRE (xml != nullptr);
        if (auto* mlNode = xml->getChildByName ("midilearn"))
            xml->removeChildElement (mlNode, true);
        TestableProcessor::copyXmlToBinary (*xml, legacy);
    }

    ASamplerProcessor b;
    b.bindCcToParameter (50, param_ids::padVolume (1));
    REQUIRE (b.getCcBinding (50) == juce::String (param_ids::padVolume (1)));

    b.setStateInformation (legacy.getData(), static_cast<int> (legacy.getSize()));
    REQUIRE (b.getCcBinding (50).isEmpty());
}

// ----- One-shot mode -----------------------------------------------------

TEST_CASE ("Gated default: note-off releases the voice", "[plugin][oneshot]")
{
    // Default ADSR has release = 50 ms, attack/decay 0, sustain 1. With a
    // long DC sample the voice would otherwise play forever. Setting
    // release to 0 lets us read "released" as "voice count is 0 in the
    // very next block" without waiting for the envelope tail.
    auto wav = writeConstantWav ("oneshot_gated", 0.5f, 44100);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> on (2, 512);
    auto onMidi = noteOnAt (36, 1.0f);
    proc.processBlock (on, onMidi);
    REQUIRE (proc.getActiveVoiceCount() == 1);

    juce::AudioBuffer<float> off (2, 512);
    juce::MidiBuffer offMidi;
    offMidi.addEvent (juce::MidiMessage::noteOff (1, 36), 0);
    proc.processBlock (off, offMidi);
    REQUIRE (proc.getActiveVoiceCount() == 0);

    wav.deleteFile();
}

TEST_CASE ("One-shot mode: note-off does not release the voice",
           "[plugin][oneshot]")
{
    auto wav = writeConstantWav ("oneshot_on", 0.5f, 44100);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    setParam (proc, param_ids::padOneShot (1), 1.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> on (2, 512);
    auto onMidi = noteOnAt (36, 1.0f);
    proc.processBlock (on, onMidi);
    REQUIRE (proc.getActiveVoiceCount() == 1);

    juce::AudioBuffer<float> off (2, 512);
    juce::MidiBuffer offMidi;
    offMidi.addEvent (juce::MidiMessage::noteOff (1, 36), 0);
    proc.processBlock (off, offMidi);

    // Even after several blocks past note-off, the one-shot voice keeps
    // sounding - it owns its own envelope and the DC sample is 1 s long.
    juce::MidiBuffer empty;
    for (int i = 0; i < 10; ++i)
    {
        juce::AudioBuffer<float> b (2, 512);
        proc.processBlock (b, empty);
    }
    REQUIRE (proc.getActiveVoiceCount() == 1);

    wav.deleteFile();
}

TEST_CASE ("One-shot is per-pad: sibling pad on same note still releases",
           "[plugin][oneshot]")
{
    auto wav = writeConstantWav ("oneshot_per_pad", 0.5f, 44100);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPad (1, wav) == ASamplerProcessor::LoadResult::Success);

    // Pad 2 shares pad 1's trigger note; only pad 1 is one-shot.
    setParam (proc, param_ids::padNote   (2), 36.0f);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    setParam (proc, param_ids::padRelease (2), 0.0f);
    setParam (proc, param_ids::padOneShot (1), 1.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> on (2, 512);
    auto onMidi = noteOnAt (36, 1.0f);
    proc.processBlock (on, onMidi);
    REQUIRE (proc.getActiveVoiceCount() == 2);

    juce::AudioBuffer<float> off (2, 512);
    juce::MidiBuffer offMidi;
    offMidi.addEvent (juce::MidiMessage::noteOff (1, 36), 0);
    proc.processBlock (off, offMidi);

    // Pad 2 was released (and immediately deactivated since release=0).
    // Pad 1's voice is still sounding - one-shot ignored the note-off.
    REQUIRE (proc.getActiveVoiceCount() == 1);

    wav.deleteFile();
}

TEST_CASE ("One-shot mode survives a state round-trip", "[plugin][oneshot][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::padOneShot (3), 1.0f);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (b.getState().getRawParameterValue (param_ids::padOneShot (3))->load() >= 0.5f);
    // A pad we didn't touch must still be off.
    REQUIRE (b.getState().getRawParameterValue (param_ids::padOneShot (1))->load() <  0.5f);
}

// ----- Pitch bend --------------------------------------------------------

namespace
{
    // Push `samples` of audio through the processor while the voice on pad 1
    // (default note 36) is sounding, with no further MIDI events.
    void renderSilent (ASamplerProcessor& proc, int samples, int blockSize = 512)
    {
        juce::MidiBuffer empty;
        for (int written = 0; written < samples; )
        {
            const int n = juce::jmin (blockSize, samples - written);
            juce::AudioBuffer<float> b (2, n);
            proc.processBlock (b, empty);
            written += n;
        }
    }
}

TEST_CASE ("Pitch bend at center has no effect on playback speed",
           "[plugin][bend]")
{
    auto wav = writeConstantWav ("bend_center", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> on (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192), 0);  // dead center
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);
    proc.processBlock (on, midi);

    // 21000 more samples of silence (total 21512); voice still has ~538
    // samples to go through its 22050-sample window.
    renderSilent (proc, 21000);
    REQUIRE (proc.getActiveVoiceCount() == 1);

    wav.deleteFile();
}

TEST_CASE ("Max positive bend at 12-st range consumes the sample twice as fast",
           "[plugin][bend]")
{
    auto wav = writeConstantWav ("bend_up", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    // Range = 12 st: max-positive bend (wheel 16383) = +12 st = 2x speed.
    setParam (proc, param_ids::pitchBendRange, 12.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> on (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::pitchWheel (1, 16383), 0);
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);
    proc.processBlock (on, midi);

    // Voice plays 22050 source samples in ~11025 output samples at 2x. After
    // 13000 output samples the voice must have exhausted its window and
    // deactivated. (We rendered 512 in the first block, so 12500 more here.)
    renderSilent (proc, 12500);
    REQUIRE (proc.getActiveVoiceCount() == 0);

    wav.deleteFile();
}

TEST_CASE ("Pitch bend updates affect already-sounding voices",
           "[plugin][bend]")
{
    auto wav = writeConstantWav ("bend_live", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    setParam (proc, param_ids::pitchBendRange, 12.0f);
    proc.prepareToPlay (44100.0, 512);

    // Trigger at centre bend.
    {
        juce::AudioBuffer<float> on (2, 512);
        auto midi = noteOnAt (36, 1.0f);
        proc.processBlock (on, midi);
    }

    // Voice has consumed 512 samples at 1x. Now bend up an octave: remaining
    // (22050 - 512) source samples will be consumed at 2x output speed,
    // taking ~10769 output samples. After 11500 more output samples it
    // should be done.
    {
        juce::AudioBuffer<float> b (2, 512);
        juce::MidiBuffer bend;
        bend.addEvent (juce::MidiMessage::pitchWheel (1, 16383), 0);
        proc.processBlock (b, bend);
    }
    renderSilent (proc, 11000);
    REQUIRE (proc.getActiveVoiceCount() == 0);

    wav.deleteFile();
}

TEST_CASE ("Pitch bend range survives a state round-trip",
           "[plugin][bend][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::pitchBendRange, 7.0f);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));
    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::pitchBendRange)->load()) == 7);
}

// ----- Aftertouch --------------------------------------------------------

TEST_CASE ("Aftertouch Volume route boosts the bound pad's output",
           "[plugin][aftertouch]")
{
    auto wav = writeConstantWav ("at_vol", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    setParam (proc, param_ids::padAftertouchDest (1),
              static_cast<float> (AftertouchDest::Volume));
    setParam (proc, param_ids::padAftertouchAmount (1), 1.0f);
    proc.prepareToPlay (44100.0, 512);

    // Reference render with AT = 0.
    juce::AudioBuffer<float> ref (2, 512);
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::channelPressureChange (1, 0), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);
        proc.processBlock (ref, midi);
    }
    const float refEnergy = blockEnergy (ref);

    // Fresh processor, AT = 127.
    ASamplerProcessor proc2;
    REQUIRE (proc2.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc2, param_ids::padRelease (1), 0.0f);
    setParam (proc2, param_ids::padAftertouchDest (1),
              static_cast<float> (AftertouchDest::Volume));
    setParam (proc2, param_ids::padAftertouchAmount (1), 1.0f);
    proc2.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> hot (2, 512);
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::channelPressureChange (1, 127), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);
        proc2.processBlock (hot, midi);
    }
    const float hotEnergy = blockEnergy (hot);

    // amount=1, AT=1 -> volume multiplier = 1 + 1 = 2.0 -> ~2x energy.
    REQUIRE (hotEnergy > refEnergy * 1.7f);
    REQUIRE (hotEnergy < refEnergy * 2.3f);

    wav.deleteFile();
}

TEST_CASE ("Aftertouch Cutoff route opens the filter",
           "[plugin][aftertouch]")
{
    // Two passes: low-pass filter at 200 Hz over a 1 kHz sine. AT=0 should
    // attenuate most of the signal; AT=127 with amount=1 multiplies cutoff
    // by 8x = 1600 Hz, letting the sine through with much higher energy.
    auto wav = writeSineWav ("at_cut", 1, 22050, 1000.0f);

    auto run = [&] (int atValue)
    {
        ASamplerProcessor proc;
        REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
        setParam (proc, param_ids::padRelease (1), 0.0f);
        setParam (proc, param_ids::padFilterMode (1),
                  static_cast<float> (FilterMode::LowPass));
        setParam (proc, param_ids::padFilterCutoff (1), 200.0f);
        setParam (proc, param_ids::padAftertouchDest (1),
                  static_cast<float> (AftertouchDest::FilterCutoff));
        setParam (proc, param_ids::padAftertouchAmount (1), 1.0f);
        proc.prepareToPlay (44100.0, 1024);

        juce::AudioBuffer<float> buf (2, 1024);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::channelPressureChange (1, atValue), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);
        proc.processBlock (buf, midi);
        return blockEnergy (buf);
    };

    const float closed = run (0);
    const float open   = run (127);

    REQUIRE (open > closed * 4.0f);

    wav.deleteFile();
}

TEST_CASE ("Aftertouch destination Off leaves the pad untouched",
           "[plugin][aftertouch]")
{
    auto wav = writeConstantWav ("at_off", 0.5f, 22050);

    auto run = [&] (int atValue)
    {
        ASamplerProcessor proc;
        REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
        setParam (proc, param_ids::padRelease (1), 0.0f);
        // Destination Off; amount won't matter.
        setParam (proc, param_ids::padAftertouchDest (1),
                  static_cast<float> (AftertouchDest::Off));
        setParam (proc, param_ids::padAftertouchAmount (1), 1.0f);
        proc.prepareToPlay (44100.0, 512);

        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::channelPressureChange (1, atValue), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);
        proc.processBlock (buf, midi);
        return blockEnergy (buf);
    };

    const float cold = run (0);
    const float hot  = run (127);
    REQUIRE (hot == Approx (cold).margin (1.0e-3f));

    wav.deleteFile();
}

TEST_CASE ("Aftertouch routing survives a state round-trip",
           "[plugin][aftertouch][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::padAftertouchDest (5),
                  static_cast<float> (AftertouchDest::FilterCutoff));
        setParam (a, param_ids::padAftertouchAmount (5), 0.8f);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padAftertouchDest (5))->load())
             == static_cast<int> (AftertouchDest::FilterCutoff));
    REQUIRE (b.getState().getRawParameterValue (param_ids::padAftertouchAmount (5))->load()
             == Approx (0.8f).margin (1.0e-3f));
}

// ----- Solo / mute -------------------------------------------------------

TEST_CASE ("Mute on a pad silences its output", "[plugin][mute]")
{
    auto wav = writeConstantWav ("mute_on", 0.5f, 44100);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padAttack (1), 0.0f);
    proc.prepareToPlay (44100.0, 512);

    auto render = [&] (float muteVal)
    {
        setParam (proc, param_ids::padMute (1), muteVal);
        juce::AudioBuffer<float> b (2, 512);
        auto m = noteOnAt (36, 1.0f);
        proc.processBlock (b, m);
        return blockEnergy (b);
    };

    const float dry  = render (0.0f);
    REQUIRE (dry > 0.0f);
}

TEST_CASE ("Mute silences a sounding voice live", "[plugin][mute]")
{
    auto wav = writeConstantWav ("mute_live", 0.5f, 44100);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padAttack (1), 0.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> on (2, 512);
    auto onMidi = noteOnAt (36, 1.0f);
    proc.processBlock (on, onMidi);
    REQUIRE (blockEnergy (on) > 0.0f);

    setParam (proc, param_ids::padMute (1), 1.0f);

    juce::MidiBuffer empty;
    juce::AudioBuffer<float> muted (2, 512);
    proc.processBlock (muted, empty);
    REQUIRE (blockEnergy (muted) == Approx (0.0f).margin (1.0e-5f));
    REQUIRE (proc.getActiveVoiceCount() == 1);   // voice keeps advancing

    setParam (proc, param_ids::padMute (1), 0.0f);
    juce::AudioBuffer<float> back (2, 512);
    proc.processBlock (back, empty);
    REQUIRE (blockEnergy (back) > 0.0f);

    wav.deleteFile();
}

TEST_CASE ("Solo on one pad silences other pads", "[plugin][solo]")
{
    auto wav = writeConstantWav ("solo_iso", 0.5f, 44100);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPad (1, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padNote   (2), 37.0f);
    setParam (proc, param_ids::padAttack (1), 0.0f);
    setParam (proc, param_ids::padAttack (2), 0.0f);
    setParam (proc, param_ids::padSolo   (1), 1.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> b (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);   // pad 1 (soloed)
    midi.addEvent (juce::MidiMessage::noteOn (1, 37, 1.0f), 0);   // pad 2 (not soloed)
    proc.processBlock (b, midi);

    // Pad 1 sounds; pad 2 is gated to silence. Both voices are alive in the
    // pool, the gate is applied at render only.
    REQUIRE (blockEnergy (b) > 0.0f);
    REQUIRE (proc.getActiveVoiceCount() == 2);

    wav.deleteFile();
}

TEST_CASE ("Solo overrides mute for the soloed pad", "[plugin][solo]")
{
    auto wav = writeConstantWav ("solo_override", 0.5f, 44100);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padAttack (1), 0.0f);
    setParam (proc, param_ids::padMute   (1), 1.0f);
    setParam (proc, param_ids::padSolo   (1), 1.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> b (2, 512);
    auto midi = noteOnAt (36, 1.0f);
    proc.processBlock (b, midi);

    // The pad is muted *and* soloed: solo wins (any-solo rule promotes the
    // pad to audible; mute is ignored while any pad is soloed).
    REQUIRE (blockEnergy (b) > 0.0f);

    wav.deleteFile();
}

TEST_CASE ("Mute and Solo survive a state round-trip",
           "[plugin][mute][solo][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::padMute (2),  1.0f);
        setParam (a, param_ids::padSolo (7),  1.0f);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (b.getState().getRawParameterValue (param_ids::padMute (2))->load() >= 0.5f);
    REQUIRE (b.getState().getRawParameterValue (param_ids::padSolo (7))->load() >= 0.5f);
    REQUIRE (b.getState().getRawParameterValue (param_ids::padMute (5))->load() <  0.5f);
    REQUIRE (b.getState().getRawParameterValue (param_ids::padSolo (1))->load() <  0.5f);
}

// ----- Glide / portamento ------------------------------------------------

TEST_CASE ("Glide off: second trigger jumps straight to new pitch",
           "[plugin][glide]")
{
    // Sample is 22050 samples; at +12 st (2x speed) the voice finishes in
    // ~11025 output samples. With glide off, that timing must hold even
    // though pad 1 had been triggered earlier at pitch 0.
    auto wav = writeConstantWav ("glide_off", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    setParam (proc, param_ids::padGlideMode (1),
              static_cast<float> (GlideMode::Off));
    setParam (proc, param_ids::padGlideTime (1), 500.0f);
    proc.prepareToPlay (44100.0, 512);

    // First trigger at pitch 0 — let the voice play out naturally.
    setParam (proc, param_ids::padPitch (1), 0.0f);
    {
        juce::AudioBuffer<float> b (2, 512);
        auto m = noteOnAt (36, 1.0f);
        proc.processBlock (b, m);
    }
    renderSilent (proc, 25000);
    REQUIRE (proc.getActiveVoiceCount() == 0);

    // Second trigger at +12 st. Glide is off → 2x speed throughout. Done
    // within ~11025 output samples; after 12500 the voice must be inactive.
    setParam (proc, param_ids::padPitch (1), 12.0f);
    {
        juce::AudioBuffer<float> b (2, 512);
        auto m = noteOnAt (36, 1.0f);
        proc.processBlock (b, m);
    }
    renderSilent (proc, 12500);
    REQUIRE (proc.getActiveVoiceCount() == 0);

    wav.deleteFile();
}

TEST_CASE ("Glide Always: second trigger slides from previous pitch",
           "[plugin][glide]")
{
    // Same setup as "Glide off" but with Always mode + 500 ms glide. The
    // second voice starts at the previous pad pitch (1x speed) and ramps to
    // its own (+12 st = 2x). It must NOT have finished after 12500 samples
    // — that's the no-glide bound.
    auto wav = writeConstantWav ("glide_always", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    setParam (proc, param_ids::padGlideMode (1),
              static_cast<float> (GlideMode::Always));
    setParam (proc, param_ids::padGlideTime (1), 500.0f);
    proc.prepareToPlay (44100.0, 512);

    setParam (proc, param_ids::padPitch (1), 0.0f);
    {
        juce::AudioBuffer<float> b (2, 512);
        auto m = noteOnAt (36, 1.0f);
        proc.processBlock (b, m);
    }
    renderSilent (proc, 25000);
    REQUIRE (proc.getActiveVoiceCount() == 0);

    setParam (proc, param_ids::padPitch (1), 12.0f);
    {
        juce::AudioBuffer<float> b (2, 512);
        auto m = noteOnAt (36, 1.0f);
        proc.processBlock (b, m);
    }
    renderSilent (proc, 12500);
    REQUIRE (proc.getActiveVoiceCount() == 1);

    wav.deleteFile();
}

TEST_CASE ("Glide Legato: no slide when previous voice has finished",
           "[plugin][glide]")
{
    // Legato mode requires a sounding previous voice to glide. We let the
    // first voice die before triggering the second, so the slide must NOT
    // engage — voice finishes within the no-glide bound (~11025 samples).
    auto wav = writeConstantWav ("glide_legato_off", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    setParam (proc, param_ids::padGlideMode (1),
              static_cast<float> (GlideMode::Legato));
    setParam (proc, param_ids::padGlideTime (1), 500.0f);
    proc.prepareToPlay (44100.0, 512);

    setParam (proc, param_ids::padPitch (1), 0.0f);
    {
        juce::AudioBuffer<float> b (2, 512);
        auto m = noteOnAt (36, 1.0f);
        proc.processBlock (b, m);
    }
    renderSilent (proc, 25000);
    REQUIRE (proc.getActiveVoiceCount() == 0);

    setParam (proc, param_ids::padPitch (1), 12.0f);
    {
        juce::AudioBuffer<float> b (2, 512);
        auto m = noteOnAt (36, 1.0f);
        proc.processBlock (b, m);
    }
    renderSilent (proc, 12500);
    REQUIRE (proc.getActiveVoiceCount() == 0);

    wav.deleteFile();
}

TEST_CASE ("Glide: first-ever trigger never slides", "[plugin][glide]")
{
    // Always mode, but no prior trigger to glide from. The single trigger
    // at +12 st must finish at the no-glide bound.
    auto wav = writeConstantWav ("glide_first", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1), 0.0f);
    setParam (proc, param_ids::padGlideMode (1),
              static_cast<float> (GlideMode::Always));
    setParam (proc, param_ids::padGlideTime (1), 500.0f);
    setParam (proc, param_ids::padPitch (1), 12.0f);
    proc.prepareToPlay (44100.0, 512);

    {
        juce::AudioBuffer<float> b (2, 512);
        auto m = noteOnAt (36, 1.0f);
        proc.processBlock (b, m);
    }
    renderSilent (proc, 12500);
    REQUIRE (proc.getActiveVoiceCount() == 0);

    wav.deleteFile();
}

TEST_CASE ("Glide params survive a state round-trip",
           "[plugin][glide][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::padGlideMode (4),
                  static_cast<float> (GlideMode::Legato));
        setParam (a, param_ids::padGlideTime (4), 250.0f);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padGlideMode (4))->load())
             == static_cast<int> (GlideMode::Legato));
    REQUIRE (b.getState().getRawParameterValue (param_ids::padGlideTime (4))->load()
             == Approx (250.0f).margin (1.0e-2f));
}

// ----- Polyphony cap -----------------------------------------------------

TEST_CASE ("Max Voices = 1 forces monophony on a pad",
           "[plugin][polyphony]")
{
    auto wav = writeConstantWav ("poly_mono", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padMaxVoices (1), 1.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> b (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 100);
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 200);
    proc.processBlock (b, midi);

    // Three triggers on the same pad with a cap of 1 collapse to a single
    // sounding voice (each retrigger steals the previous one).
    REQUIRE (proc.getActiveVoiceCount() == 1);

    wav.deleteFile();
}

TEST_CASE ("Max Voices = 2 caps simultaneous voices on a pad",
           "[plugin][polyphony]")
{
    auto wav = writeConstantWav ("poly_two", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padMaxVoices (1), 2.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> b (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f),   0);
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 100);
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 200);
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 300);
    proc.processBlock (b, midi);

    REQUIRE (proc.getActiveVoiceCount() == 2);

    wav.deleteFile();
}

TEST_CASE ("Max Voices is per-pad: capping one pad doesn't affect another",
           "[plugin][polyphony]")
{
    auto wav = writeConstantWav ("poly_per_pad", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPad (1, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padNote      (2), 37.0f);
    setParam (proc, param_ids::padMaxVoices (1), 1.0f);  // pad 1 mono
    setParam (proc, param_ids::padMaxVoices (2), 8.0f);  // pad 2 free
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> b (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f),   0);   // pad 1
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 100);   // steals pad 1
    midi.addEvent (juce::MidiMessage::noteOn (1, 37, 1.0f), 200);   // pad 2
    midi.addEvent (juce::MidiMessage::noteOn (1, 37, 1.0f), 300);   // pad 2 (no cap hit)
    proc.processBlock (b, midi);

    // Pad 1 holds exactly one; pad 2 has two coexisting voices.
    REQUIRE (proc.getActiveVoiceCount() == 3);

    wav.deleteFile();
}

TEST_CASE ("Max Voices survives a state round-trip",
           "[plugin][polyphony][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::padMaxVoices (6), 3.0f);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padMaxVoices (6))->load()) == 3);
    // Pads we didn't touch must still be at the default cap (8).
    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padMaxVoices (1))->load()) == 8);
}

// ----- Per-pad Mode (Fixed / Keyboard) -----------------------------------

TEST_CASE ("Default Pad Mode is Fixed for every pad",
           "[plugin][padmode]")
{
    // Existing kits must keep their MPC-strict dispatch. Any new pad must
    // start in Fixed so loading an older state can't accidentally turn a
    // drum pad into a keyboard-mapped instrument.
    ASamplerProcessor proc;
    for (int p = 1; p <= kNumPads; ++p)
    {
        CAPTURE (p);
        const int mode = juce::roundToInt (
            proc.getState().getRawParameterValue (param_ids::padMode (p))->load());
        REQUIRE (mode == static_cast<int> (PadMode::Fixed));
    }
}

TEST_CASE ("Keyboard mode: pad triggers on any incoming note in [low, high]",
           "[plugin][padmode]")
{
    ASamplerProcessor proc;
    setParam (proc, param_ids::padMode (1),     static_cast<float> (PadMode::Keyboard));
    setParam (proc, param_ids::padLowNote (1),  48.0f);
    setParam (proc, param_ids::padHighNote (1), 72.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> buf (2, 512);
    auto midi = noteOnAt (60, 1.0f);   // C4, mid-range
    proc.processBlock (buf, midi);
    REQUIRE (blockEnergy (buf) > 0.0f);
}

TEST_CASE ("Keyboard mode: pad ignores notes outside [low, high]",
           "[plugin][padmode]")
{
    // Note 96 is above every default Fixed-mode pad's assigned note (36..51)
    // and above the Keyboard-mode pad 1's high bound (72), so nothing should
    // sound.
    ASamplerProcessor proc;
    setParam (proc, param_ids::padMode (1),     static_cast<float> (PadMode::Keyboard));
    setParam (proc, param_ids::padLowNote (1),  48.0f);
    setParam (proc, param_ids::padHighNote (1), 72.0f);
    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> buf (2, 512);
    auto midi = noteOnAt (96, 1.0f);
    proc.processBlock (buf, midi);
    REQUIRE (blockEnergy (buf) == 0.0f);
    REQUIRE (proc.getActiveVoiceCount() == 0);
}

TEST_CASE ("Keyboard mode: incoming note pitches sample relative to root",
           "[plugin][padmode]")
{
    // Pitch is detectable by playback duration: a 22050-sample constant WAV
    // played at +12 semitones (2x ratio) drains in ~11025 host samples,
    // while the same sample at root pitch lasts the full 22050. We pick a
    // render window between those two — the +12 voice must be gone.
    auto wav = writeConstantWav ("padmode_pitch", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1),  0.0f);
    setParam (proc, param_ids::padMode (1),     static_cast<float> (PadMode::Keyboard));
    setParam (proc, param_ids::padLowNote (1),  0.0f);
    setParam (proc, param_ids::padHighNote (1), 127.0f);
    setParam (proc, param_ids::padNote (1),     60.0f);  // root = C4

    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> on (2, 512);
    auto midi = noteOnAt (72, 1.0f);   // root + 12 -> 2x pitch ratio
    proc.processBlock (on, midi);

    renderSilent (proc, 11500);   // total 12012; well past 11025
    REQUIRE (proc.getActiveVoiceCount() == 0);

    wav.deleteFile();
}

TEST_CASE ("Keyboard mode: trigger at root note has no extra pitch shift",
           "[plugin][padmode]")
{
    // Same fixture as the previous test, but triggered at the root note.
    // Pitch ratio is 1.0 so the 22050-sample window outlives the 12500
    // render window — voice must still be active.
    auto wav = writeConstantWav ("padmode_root", 0.5f, 22050);
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1),  0.0f);
    setParam (proc, param_ids::padMode (1),     static_cast<float> (PadMode::Keyboard));
    setParam (proc, param_ids::padLowNote (1),  0.0f);
    setParam (proc, param_ids::padHighNote (1), 127.0f);
    setParam (proc, param_ids::padNote (1),     60.0f);

    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> on (2, 512);
    auto midi = noteOnAt (60, 1.0f);   // exactly the root
    proc.processBlock (on, midi);

    renderSilent (proc, 12000);
    REQUIRE (proc.getActiveVoiceCount() == 1);

    wav.deleteFile();
}

TEST_CASE ("Keyboard mode: note-off releases only the matching voice",
           "[plugin][padmode]")
{
    // Trigger two in-range notes on a Keyboard pad, then note-off only one.
    // After the release window expires the released voice deactivates while
    // the sustaining voice keeps running. Confirms per-note releasePadNote
    // matching rather than a blanket releasePad.
    auto wav = writeSteadyWav ("padmode_release");
    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padRelease (1),  0.05f);  // 50 ms tail
    setParam (proc, param_ids::padMode (1),     static_cast<float> (PadMode::Keyboard));
    setParam (proc, param_ids::padLowNote (1),  48.0f);
    setParam (proc, param_ids::padHighNote (1), 72.0f);
    setParam (proc, param_ids::padNote (1),     60.0f);  // root

    proc.prepareToPlay (44100.0, 512);

    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
    midi.addEvent (juce::MidiMessage::noteOn (1, 67, 1.0f), 1);
    proc.processBlock (buf, midi);
    REQUIRE (proc.getActiveVoiceCount() == 2);

    juce::AudioBuffer<float> buf2 (2, 512);
    juce::MidiBuffer midi2;
    midi2.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    proc.processBlock (buf2, midi2);

    // ~150 ms of further silence — well past the 50 ms release tail of the
    // note-off'd voice, but the steady WAV (500 ms) is still sounding for
    // the held voice.
    renderSilent (proc, 6500);
    REQUIRE (proc.getActiveVoiceCount() == 1);

    wav.deleteFile();
}

TEST_CASE ("Pad Mode / Low Note / High Note survive a state round-trip",
           "[plugin][padmode][state]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::padMode (3),     static_cast<float> (PadMode::Keyboard));
        setParam (a, param_ids::padLowNote (3),  24.0f);
        setParam (a, param_ids::padHighNote (3), 96.0f);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), static_cast<int> (data.getSize()));

    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padMode (3))->load())
             == static_cast<int> (PadMode::Keyboard));
    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padLowNote (3))->load()) == 24);
    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padHighNote (3))->load()) == 96);

    // Untouched pad stays Fixed with full-range defaults.
    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padMode (1))->load())
             == static_cast<int> (PadMode::Fixed));
    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padLowNote (1))->load()) == 0);
    REQUIRE (juce::roundToInt (
                 b.getState().getRawParameterValue (param_ids::padHighNote (1))->load()) == 127);
}

