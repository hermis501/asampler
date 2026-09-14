#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginProcessor.h"

using namespace asampler;

namespace
{
    // Write a short WAV so tests can exercise real load / kit / relink paths.
    juce::File writeTempWav (const juce::String& name, float amplitude = 0.5f)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("ASamplerTests");
        dir.createDirectory();
        const auto file = dir.getChildFile (name);
        file.deleteFile();

        juce::AudioBuffer<float> buf (1, 1000);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            buf.setSample (0, i, amplitude * std::sin (i * 0.05f));

        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
            return {};

        std::unique_ptr<juce::AudioFormatWriter> writer (
            fmt.createWriterFor (stream.release(), 44100.0, 1, 16, {}, 0));
        if (writer == nullptr)
            return {};

        writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
        writer.reset();
        return file;
    }

    float plainValue (ASamplerProcessor& proc, const juce::String& id)
    {
        auto* p = proc.getState().getParameter (id);
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    }

    // Write through the APVTS ValueTree, which is what the plugin's own
    // setRangedParam does. Writing the parameter object directly only reaches
    // the tree later on APVTS's internal timer, so undo assertions made
    // immediately afterwards would compare against a stale tree value.
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
}

// ---------------- Tail length ----------------

TEST_CASE ("Tail length is never zero", "[plugin][tail]")
{
    ASamplerProcessor proc;

    // A zero tail tells the host nothing sounds after the last note, and
    // release tails get truncated on render.
    REQUIRE (proc.getTailLengthSeconds() >= 1.0);
}

TEST_CASE ("Tail length tracks the longest release", "[plugin][tail]")
{
    ASamplerProcessor proc;
    const double before = proc.getTailLengthSeconds();

    setPlain (proc, param_ids::padRelease (5), 4.0f);
    REQUIRE (proc.getTailLengthSeconds() >= 4.0);
    REQUIRE (proc.getTailLengthSeconds() > before);
}

// ---------------- Focus proxy ----------------

TEST_CASE ("Focused Pad parameter exists and defaults to Pad 1", "[plugin][focus]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getState().getRawParameterValue (param_ids::focusedPad) != nullptr);
    REQUIRE (proc.getCurrentFocusedPad() == 0);
}

TEST_CASE ("Focus proxies read the focused pad", "[plugin][focus]")
{
    ASamplerProcessor proc;

    setPlain (proc, param_ids::padVolume (1), -10.0f);
    setPlain (proc, param_ids::padVolume (4), 6.0f);

    // Find the "Volume" proxy by name among the processor's parameters.
    auto findProxy = [&proc] (const juce::String& name) -> juce::AudioProcessorParameter*
    {
        for (auto* p : proc.getParameters())
            if (dynamic_cast<FocusProxyParameter*> (p) != nullptr
                && p->getName (64) == name)
                return p;
        return nullptr;
    };

    auto* volProxy = findProxy ("Volume");
    REQUIRE (volProxy != nullptr);

    auto* padVol1 = proc.getState().getParameter (param_ids::padVolume (1));
    auto* padVol4 = proc.getState().getParameter (param_ids::padVolume (4));
    REQUIRE (padVol1 != nullptr);
    REQUIRE (padVol4 != nullptr);

    // Focus is Pad 1 by default.
    REQUIRE (volProxy->getValue() == padVol1->getValue());

    setPlain (proc, param_ids::focusedPad, 4.0f);
    REQUIRE (proc.getCurrentFocusedPad() == 3);
    REQUIRE (volProxy->getValue() == padVol4->getValue());
}

TEST_CASE ("Focus proxy writes through to the focused pad", "[plugin][focus]")
{
    ASamplerProcessor proc;
    setPlain (proc, param_ids::focusedPad, 7.0f);

    juce::AudioProcessorParameter* volProxy = nullptr;
    for (auto* p : proc.getParameters())
        if (dynamic_cast<FocusProxyParameter*> (p) != nullptr && p->getName (64) == "Volume")
            volProxy = p;

    REQUIRE (volProxy != nullptr);

    auto* target = proc.getState().getParameter (param_ids::padVolume (7));
    REQUIRE (target != nullptr);

    volProxy->setValueNotifyingHost (target->convertTo0to1 (-18.0f));
    REQUIRE (plainValue (proc, param_ids::padVolume (7)) == Catch::Approx (-18.0f).margin (0.05));

    // Pad 1 must be untouched — the proxy targets only the focused pad.
    REQUIRE (plainValue (proc, param_ids::padVolume (1)) == Catch::Approx (0.0f).margin (0.05));
}

TEST_CASE ("Focus proxy formats text like the pad parameter", "[plugin][focus][accessibility]")
{
    // Screen readers speak this string, so it must be the human-readable form
    // ("-6.00 dB"), not a raw normalised float.
    ASamplerProcessor proc;

    juce::AudioProcessorParameter* volProxy = nullptr;
    for (auto* p : proc.getParameters())
        if (dynamic_cast<FocusProxyParameter*> (p) != nullptr && p->getName (64) == "Volume")
            volProxy = p;

    REQUIRE (volProxy != nullptr);

    auto* target = proc.getState().getParameter (param_ids::padVolume (1));
    const float norm = target->convertTo0to1 (-6.0f);
    REQUIRE (volProxy->getText (norm, 32).contains ("dB"));
}

TEST_CASE ("There is one focus proxy per pad parameter", "[plugin][focus]")
{
    ASamplerProcessor proc;

    int proxies = 0;
    for (auto* p : proc.getParameters())
        if (dynamic_cast<FocusProxyParameter*> (p) != nullptr)
            ++proxies;

    // Every per-pad control must be reachable from the Focus group, otherwise
    // it is only findable by scrolling the full per-pad list.
    REQUIRE (proxies > 50);

    // And each pad group should expose the same count.
    int padOneParams = 0;
    for (auto* p : proc.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            if (withId->paramID.startsWith ("pad_1_"))
                ++padOneParams;

    REQUIRE (proxies == padOneParams);
}

// ---------------- Tempo sync ----------------

TEST_CASE ("Tempo sync parameters exist with sensible defaults", "[plugin][tempo]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padTempoSync (1)) != nullptr);
    REQUIRE (plainValue (proc, param_ids::padTempoSync (1)) < 0.5f);           // off
    REQUIRE (plainValue (proc, param_ids::padOriginalBpm (1)) == Catch::Approx (120.0f));
}

// ---------------- Layer gain / normalise ----------------

TEST_CASE ("Layer gain parameters exist for every layer", "[plugin][layergain]")
{
    ASamplerProcessor proc;
    for (int L = 1; L <= kNumLayers; ++L)
    {
        REQUIRE (proc.getState().getRawParameterValue (param_ids::padLayerGain (1, L)) != nullptr);
        REQUIRE (plainValue (proc, param_ids::padLayerGain (1, L)) == Catch::Approx (0.0f));
    }
}

TEST_CASE ("Normalise sets the layer gain from the sample peak", "[plugin][normalise]")
{
    ASamplerProcessor proc;

    const auto wav = writeTempWav ("normalise.wav", 0.25f);
    REQUIRE (wav.existsAsFile());
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav) == ASamplerProcessor::LoadResult::Success);

    REQUIRE (proc.normaliseLayer (0, 0));

    // A 0.25 peak needs roughly +11 dB to reach -1 dBFS.
    const float gain = plainValue (proc, param_ids::padLayerGain (1, 1));
    REQUIRE (gain > 8.0f);
    REQUIRE (gain < 14.0f);
}

// ---------------- Missing samples / relink ----------------

TEST_CASE ("A missing sample is reported rather than silently dropped",
           "[plugin][relink]")
{
    const auto wav = writeTempWav ("relink_me.wav");
    REQUIRE (wav.existsAsFile());

    juce::MemoryBlock stateBlock;
    {
        ASamplerProcessor proc;
        REQUIRE (proc.loadSampleForPadLayer (2, 0, wav)
                 == ASamplerProcessor::LoadResult::Success);
        proc.getStateInformation (stateBlock);
    }

    // Move the file so the saved path no longer resolves, as if the project
    // had been opened on another machine.
    const auto moved = wav.getParentDirectory().getChildFile ("moved_relink_me.wav");
    moved.deleteFile();
    REQUIRE (wav.moveFileTo (moved));

    ASamplerProcessor proc;
    proc.setStateInformation (stateBlock.getData(), (int) stateBlock.getSize());

    REQUIRE (proc.getMissingSampleCount() == 1);
    REQUIRE (proc.getPadSampleDisplayName (2).contains ("missing"));

    // Relinking by name from the folder repairs it.
    const auto restored = moved.getParentDirectory().getChildFile ("relink_me.wav");
    restored.deleteFile();
    REQUIRE (moved.moveFileTo (restored));

    REQUIRE (proc.relinkMissingSamples (restored.getParentDirectory()) == 1);
    REQUIRE (proc.getMissingSampleCount() == 0);
    REQUIRE_FALSE (proc.getPadSampleDisplayName (2).contains ("missing"));
}

// ---------------- Kits ----------------

TEST_CASE ("A kit round-trips settings and sample paths", "[plugin][kit]")
{
    const auto wav = writeTempWav ("kit_sample.wav");
    REQUIRE (wav.existsAsFile());

    const auto kitFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("ASamplerTests").getChildFile ("test.asamplerkit");
    kitFile.deleteFile();

    {
        ASamplerProcessor proc;
        setPlain (proc, param_ids::padVolume (3), -7.5f);
        REQUIRE (proc.loadSampleForPadLayer (1, 0, wav)
                 == ASamplerProcessor::LoadResult::Success);
        REQUIRE (proc.saveKitToFile (kitFile));
    }

    REQUIRE (kitFile.existsAsFile());

    ASamplerProcessor proc;
    REQUIRE (proc.loadKitFromFile (kitFile));
    REQUIRE (plainValue (proc, param_ids::padVolume (3)) == Catch::Approx (-7.5f).margin (0.05));
    REQUIRE (proc.getPadSampleDisplayName (1) == juce::String ("kit_sample.wav"));
}

TEST_CASE ("Loading a non-kit file fails cleanly", "[plugin][kit]")
{
    ASamplerProcessor proc;

    const auto bogus = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("ASamplerTests").getChildFile ("not_a_kit.txt");
    bogus.getParentDirectory().createDirectory();
    bogus.replaceWithText ("definitely not a kit");

    REQUIRE_FALSE (proc.loadKitFromFile (bogus));
    REQUIRE_FALSE (proc.loadKitFromFile (bogus.getSiblingFile ("does_not_exist.asamplerkit")));
}

TEST_CASE ("Factory kits apply settings", "[plugin][kit]")
{
    ASamplerProcessor proc;
    const auto names = ASamplerProcessor::getFactoryKitNames();
    REQUIRE (names.size() >= 4);

    for (int i = 0; i < names.size(); ++i)
        REQUIRE (proc.loadFactoryKit (i));

    REQUIRE_FALSE (proc.loadFactoryKit (-1));
    REQUIRE_FALSE (proc.loadFactoryKit (names.size()));

    // The 808 kit should leave every pad keyboard-playable and monophonic.
    REQUIRE (proc.loadFactoryKit (3));
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padMode (1)))
             == static_cast<int> (PadMode::Keyboard));
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padMaxVoices (1))) == 1);
}

TEST_CASE ("Factory kits set the hat choke pair", "[plugin][kit]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.loadFactoryKit (0));   // Trap

    // Pads 2 and 3 share a choke group so an open hat is cut by a closed one.
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padChokeGroup (2))) == 1);
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padChokeGroup (3))) == 1);
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padChokeGroup (5))) == 0);
}

// ---------------- Copy / paste ----------------

TEST_CASE ("Copy and paste moves pad settings", "[plugin][copypaste]")
{
    ASamplerProcessor proc;

    setPlain (proc, param_ids::padVolume (2), -9.0f);
    setPlain (proc, param_ids::padFilterCutoff (2), 800.0f);

    REQUIRE (proc.copyPad (1));    // 0-based -> Pad 2
    REQUIRE (proc.hasCopiedPad());
    REQUIRE (proc.pastePad (9));   // 0-based -> Pad 10

    REQUIRE (plainValue (proc, param_ids::padVolume (10)) == Catch::Approx (-9.0f).margin (0.05));
    REQUIRE (plainValue (proc, param_ids::padFilterCutoff (10)) == Catch::Approx (800.0f).margin (1.0));
}

TEST_CASE ("Paste with nothing copied is refused", "[plugin][copypaste]")
{
    ASamplerProcessor proc;
    REQUIRE_FALSE (proc.hasCopiedPad());
    REQUIRE_FALSE (proc.pastePad (0));
}

TEST_CASE ("Copy and paste reject out-of-range pads", "[plugin][copypaste]")
{
    ASamplerProcessor proc;
    REQUIRE_FALSE (proc.copyPad (-1));
    REQUIRE_FALSE (proc.copyPad (kNumPads));
    REQUIRE (proc.copyPad (0));
    REQUIRE_FALSE (proc.pastePad (-1));
    REQUIRE_FALSE (proc.pastePad (kNumPads));
}

TEST_CASE ("Clear all pads resets every sample", "[plugin][copypaste]")
{
    ASamplerProcessor proc;
    const auto wav = writeTempWav ("clearme.wav");
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.getPadSampleDisplayName (0) != juce::String ("default"));

    proc.clearAllPads();
    for (int i = 0; i < kNumPads; ++i)
        REQUIRE (proc.getPadSampleDisplayName (i) == juce::String ("default"));
}

// ---------------- Undo ----------------

TEST_CASE ("A factory kit load is a single undo step", "[plugin][undo]")
{
    // Goes through the real action path. APVTS copies parameter values into
    // its ValueTree on a timer, and it is that copy the UndoManager records —
    // so an undoable action has to pin the transaction boundary itself. This
    // test would pass spuriously if it wrote parameters directly.
    ASamplerProcessor proc;

    const float releaseBefore = plainValue (proc, param_ids::padRelease (1));
    const float attackBefore  = plainValue (proc, param_ids::padAttack (1));

    REQUIRE (proc.loadFactoryKit (3));   // 808 Set: long release on every pad
    REQUIRE (plainValue (proc, param_ids::padRelease (1)) > releaseBefore);

    REQUIRE (proc.undoLastAction());
    REQUIRE (plainValue (proc, param_ids::padRelease (1))
             == Catch::Approx (releaseBefore).margin (0.001));
    REQUIRE (plainValue (proc, param_ids::padAttack (1))
             == Catch::Approx (attackBefore).margin (0.001));

    REQUIRE (proc.redoLastAction());
    REQUIRE (plainValue (proc, param_ids::padRelease (1)) > releaseBefore);
}

TEST_CASE ("A chop across pads can be undone", "[plugin][undo][chop]")
{
    // The most destructive action in the plugin: it rewrites the playback
    // window on every destination pad. Being able to undo it in one step is
    // what makes it safe to use without being able to see the result.
    ASamplerProcessor proc;

    const auto wav = writeTempWav ("chop_undo.wav");
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);

    const float startBefore = plainValue (proc, param_ids::padStart (4));
    const float endBefore   = plainValue (proc, param_ids::padEnd (4));

    REQUIRE (proc.chopAcrossPads (0, 8) == ASamplerProcessor::ChopResult::Success);
    REQUIRE (plainValue (proc, param_ids::padStart (4)) != Catch::Approx (startBefore));

    REQUIRE (proc.undoLastAction());
    REQUIRE (plainValue (proc, param_ids::padStart (4))
             == Catch::Approx (startBefore).margin (0.001));
    REQUIRE (plainValue (proc, param_ids::padEnd (4))
             == Catch::Approx (endBefore).margin (0.001));
}

TEST_CASE ("Undo with an empty history is reported, not crashed", "[plugin][undo]")
{
    ASamplerProcessor proc;
    REQUIRE_FALSE (proc.undoLastAction());
}

// ---------------- Slice MIDI export ----------------

TEST_CASE ("Slice MIDI export writes a readable file", "[plugin][midiexport]")
{
    ASamplerProcessor proc;

    const auto out = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("ASamplerTests").getChildFile ("slices.mid");
    out.getParentDirectory().createDirectory();
    out.deleteFile();

    REQUIRE (proc.exportSliceMidi (out, 8));
    REQUIRE (out.existsAsFile());
    REQUIRE (out.getSize() > 0);

    juce::FileInputStream in (out);
    REQUIRE (in.openedOk());

    juce::MidiFile midi;
    REQUIRE (midi.readFrom (in));
    REQUIRE (midi.getNumTracks() >= 1);

    // Count note-ons across all tracks: one per slice.
    int noteOns = 0;
    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        const auto* seq = midi.getTrack (t);
        for (int e = 0; e < seq->getNumEvents(); ++e)
            if (seq->getEventPointer (e)->message.isNoteOn())
                ++noteOns;
    }
    REQUIRE (noteOns == 8);
}

TEST_CASE ("Slice MIDI export clamps the slice count", "[plugin][midiexport]")
{
    ASamplerProcessor proc;
    const auto out = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("ASamplerTests").getChildFile ("slices_clamped.mid");
    out.getParentDirectory().createDirectory();

    // Absurd counts must not overrun the pad array.
    REQUIRE (proc.exportSliceMidi (out, 9999));
    REQUIRE (proc.exportSliceMidi (out, -5));
}

// ---------------- Audition ----------------

TEST_CASE ("Audition triggers a voice through processBlock", "[plugin][audition]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    REQUIRE (proc.getActiveVoiceCount() == 0);

    proc.auditionPad (0);

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    proc.processBlock (buffer, midi);

    REQUIRE (proc.getActiveVoiceCount() > 0);
}

TEST_CASE ("Audition ignores out-of-range pads", "[plugin][audition]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    proc.auditionPad (-1);
    proc.auditionPad (kNumPads);

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    proc.processBlock (buffer, midi);

    REQUIRE (proc.getActiveVoiceCount() == 0);
}

TEST_CASE ("An audition request fires only once", "[plugin][audition]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    // Pad 1 defaults to one-shot off, so the voice is released at block end
    // and a second block must not re-trigger it.
    proc.auditionPad (0);

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    proc.processBlock (buffer, midi);
    const int afterFirst = proc.getActiveVoiceCount();

    proc.processBlock (buffer, midi);
    REQUIRE (proc.getActiveVoiceCount() <= afterFirst);
}
