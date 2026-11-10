// Tests for the 1.0 release work: accessible labels, time-based slice points,
// sample browsing, sample description, programs, bouncing and robustness.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

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

    float getParam (ASamplerProcessor& proc, const juce::String& id)
    {
        auto* v = proc.getState().getRawParameterValue (id);
        REQUIRE (v != nullptr);
        return v->load();
    }

    // The Focus-group proxy for a per-pad control, by its short spec name
    // ("Start", "Filter Cutoff"). Searches the whole "focus" group subtree and
    // ignores any display prefix, so it survives renaming and regrouping.
    juce::AudioProcessorParameter* findFocusProxy (ASamplerProcessor& proc, const juce::String& specName)
    {
        for (auto* node : proc.getParameterTree().getSubgroups (false))
            if (node->getID() == "focus")
                for (auto* p : node->getParameters (true))
                {
                    const auto name = p->getName (256);
                    if (name == specName || name.endsWith (" " + specName)
                                         || name.endsWith ("/" + specName))
                    {
                        // "Loop Start" also ends in " Start"; require that the
                        // text before the spec name is only a prefix word.
                        const auto before = name.dropLastCharacters (specName.length()).trim();
                        if (before.isEmpty() || before == "Focus" || before == "F")
                            return p;
                    }
                }
        return nullptr;
    }

    juce::File testDir (const juce::String& name)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("asampler_release_tests")
                       .getChildFile (name);
        dir.deleteRecursively();
        dir.createDirectory();
        return dir;
    }

    // Writes `seconds` of a sine at `freqHz` (amplitude `amp`) as a mono WAV.
    juce::File writeSine (const juce::File& file, double seconds, float freqHz = 440.0f,
                          float amp = 0.5f, double sampleRate = 44100.0)
    {
        const int n = juce::roundToInt (seconds * sampleRate);
        juce::AudioBuffer<float> buffer (1, n);
        for (int i = 0; i < n; ++i)
            buffer.setSample (0, i, amp * std::sin (2.0f * juce::MathConstants<float>::pi
                                                     * freqHz * (float) i / (float) sampleRate));

        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (new juce::FileOutputStream (file), sampleRate, 1, 16, {}, 0));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (buffer, 0, n);
        return file;
    }
}

// ----- Pad labels -----------------------------------------------------------

TEST_CASE ("Pad trigger description follows the live Note parameter",
           "[plugin][accessibility][labels]")
{
    ASamplerProcessor proc;

    // Factory default for pad 1 is C1 (MIDI 36).
    REQUIRE (proc.describePadTrigger (0) == "note C1");

    // Re-noting the pad must change what the label says.
    setParam (proc, param_ids::padNote (1), 38.0f);
    REQUIRE (proc.describePadTrigger (0) == "note D1");
}

TEST_CASE ("Keyboard-mode pads describe their key range and root",
           "[plugin][accessibility][labels]")
{
    ASamplerProcessor proc;
    setParam (proc, param_ids::padMode (2), static_cast<float> (PadMode::Keyboard));
    setParam (proc, param_ids::padLowNote (2),  36.0f);
    setParam (proc, param_ids::padHighNote (2), 72.0f);
    setParam (proc, param_ids::padNote (2),     48.0f);

    REQUIRE (proc.describePadTrigger (1) == "keys C1 to C4, root C2");
}

// ----- Slice points as time -------------------------------------------------

TEST_CASE ("Slice points read as time in the loaded sample",
           "[plugin][accessibility][slicetime]")
{
    ASamplerProcessor proc;
    const auto wav = writeSine (testDir ("slicetime").getChildFile ("two_seconds.wav"), 2.0);
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.getPadReferenceSeconds (0) == Approx (2.0).margin (1.0e-3));

    auto* start = proc.getState().getParameter (param_ids::padStart (1));
    auto* end   = proc.getState().getParameter (param_ids::padEnd (1));
    REQUIRE (start->getText (0.25f, 64) == "500.0 ms");
    REQUIRE (end->getText (1.0f, 64)    == "2.000 s");

    // The loop points use the same scale.
    auto* loopEnd = proc.getState().getParameter (param_ids::padLoopEnd (1));
    REQUIRE (loopEnd->getText (0.75f, 64) == "1.500 s");
}

TEST_CASE ("Typed slice-point values accept seconds, milliseconds and percent",
           "[plugin][accessibility][slicetime]")
{
    ASamplerProcessor proc;
    const auto wav = writeSine (testDir ("sliceparse").getChildFile ("two_seconds.wav"), 2.0);
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);

    auto* start = proc.getState().getParameter (param_ids::padStart (1));
    REQUIRE (start->getValueForText ("1.5 s")  == Approx (0.75f).margin (1.0e-4f));
    REQUIRE (start->getValueForText ("250 ms") == Approx (0.125f).margin (1.0e-4f));
    REQUIRE (start->getValueForText ("250ms")  == Approx (0.125f).margin (1.0e-4f));
    REQUIRE (start->getValueForText ("50%")    == Approx (0.5f).margin (1.0e-4f));
    REQUIRE (start->getValueForText ("1")      == Approx (0.5f).margin (1.0e-4f));   // bare = seconds

    // Past the end of the sample clamps rather than wrapping or failing.
    REQUIRE (start->getValueForText ("10 s")   == Approx (1.0f));
}

TEST_CASE ("Focus proxies show slice points as time too",
           "[plugin][accessibility][slicetime][focus]")
{
    ASamplerProcessor proc;
    const auto wav = writeSine (testDir ("slicefocus").getChildFile ("two_seconds.wav"), 2.0);
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);

    auto* focusStart = findFocusProxy (proc, "Start");
    REQUIRE (focusStart != nullptr);
    REQUIRE (focusStart->getText (0.25f, 64) == "500.0 ms");
}

// ----- Sample browsing ------------------------------------------------------

TEST_CASE ("Browsing steps through a folder in natural order and announces position",
           "[plugin][accessibility][browse]")
{
    const auto dir = testDir ("browse");
    // Deliberately created out of order; "kick 10" must sort after "kick 2".
    writeSine (dir.getChildFile ("kick 10.wav"), 0.05);
    writeSine (dir.getChildFile ("kick 2.wav"),  0.05);
    writeSine (dir.getChildFile ("kick 1.wav"),  0.05);
    dir.getChildFile ("notes.txt").replaceWithText ("not audio");

    ASamplerProcessor proc;
    const auto files = proc.listAudioFilesIn (dir);
    REQUIRE (files.size() == 3);
    REQUIRE (files[0].getFileName() == "kick 1.wav");
    REQUIRE (files[1].getFileName() == "kick 2.wav");
    REQUIRE (files[2].getFileName() == "kick 10.wav");

    REQUIRE (proc.loadSampleForPad (0, files[0]) == ASamplerProcessor::LoadResult::Success);

    REQUIRE (proc.browseSample (0, +1, true) == ASamplerProcessor::BrowseResult::Success);
    REQUIRE (proc.getPadSampleDisplayName (0) == "kick 2.wav");
    REQUIRE (proc.getLastStatusMessage() == "kick 2, 2 of 3");

    REQUIRE (proc.browseSample (0, +1, true) == ASamplerProcessor::BrowseResult::Success);
    REQUIRE (proc.getLastStatusMessage() == "kick 10, 3 of 3");

    // Wraps forward to the first file, and backward to the last.
    REQUIRE (proc.browseSample (0, +1, true) == ASamplerProcessor::BrowseResult::Success);
    REQUIRE (proc.getLastStatusMessage() == "kick 1, 1 of 3");
    REQUIRE (proc.browseSample (0, -1, true) == ASamplerProcessor::BrowseResult::Success);
    REQUIRE (proc.getLastStatusMessage() == "kick 10, 3 of 3");
}

TEST_CASE ("Browsing with nothing loaded explains what to do",
           "[plugin][accessibility][browse]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.browseSample (3, +1, true) == ASamplerProcessor::BrowseResult::NoCurrentFile);
    REQUIRE (proc.getLastStatusMessage().contains ("Load one first"));
}

TEST_CASE ("Browsing a folder with a single file reports there is nothing else",
           "[plugin][accessibility][browse]")
{
    const auto dir = testDir ("browse_single");
    const auto only = writeSine (dir.getChildFile ("only.wav"), 0.05);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, only) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.browseSample (0, +1, true) == ASamplerProcessor::BrowseResult::NoOtherFiles);
}

TEST_CASE ("Browsing from a deleted file steps from where it would sort",
           "[plugin][browse]")
{
    const auto dir = testDir ("browse_missing");
    writeSine (dir.getChildFile ("a.wav"), 0.05);
    const auto b = writeSine (dir.getChildFile ("b.wav"), 0.05);
    writeSine (dir.getChildFile ("c.wav"), 0.05);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, b) == ASamplerProcessor::LoadResult::Success);
    b.deleteFile();

    REQUIRE (proc.browseSample (0, +1, true) == ASamplerProcessor::BrowseResult::Success);
    REQUIRE (proc.getPadSampleDisplayName (0) == "c.wav");
}

// ----- Describe sample ------------------------------------------------------

TEST_CASE ("Describe sample summarises length, level, pitch and trigger",
           "[plugin][accessibility][describe]")
{
    ASamplerProcessor proc;
    const auto wav = writeSine (testDir ("describe").getChildFile ("tone a.wav"), 2.0, 440.0f, 0.5f);
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);

    const auto text = proc.describePad (0);
    CAPTURE (text);
    REQUIRE (text.startsWith ("Pad 1, tone a.wav."));
    REQUIRE (text.contains ("Length 2.000 s, 1.00 bars at 120 BPM"));   // no host: 120 BPM
    REQUIRE (text.contains ("Mono, 44.1 kHz"));
    REQUIRE (text.contains ("Peak -6.0 dB"));
    REQUIRE (text.contains ("Pitch A3, in tune"));
    REQUIRE (text.contains ("Plays on note C1"));
    REQUIRE (proc.getLastStatusMessage() == text);   // so the editor announces it
}

TEST_CASE ("Describe sample reports a trimmed window as played",
           "[plugin][accessibility][describe]")
{
    ASamplerProcessor proc;
    const auto wav = writeSine (testDir ("describe_trim").getChildFile ("tone.wav"), 2.0);
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::padStart (1), 0.25f);
    setParam (proc, param_ids::padEnd (1),   0.75f);

    const auto text = proc.describePad (0);
    CAPTURE (text);
    REQUIRE (text.contains ("Plays 500.0 ms to 1.500 s of 2.000 s"));
}

TEST_CASE ("Describe sample on an empty pad still says something useful",
           "[plugin][accessibility][describe]")
{
    ASamplerProcessor proc;
    const auto text = proc.describePad (5);
    CAPTURE (text);
    REQUIRE (text.startsWith ("Pad 6, default."));
    REQUIRE (text.contains ("Plays on note"));
}

// ----- Audition -------------------------------------------------------------

namespace
{
    // Renders `seconds` of audio in 512-sample blocks and returns the peak.
    float renderPeak (ASamplerProcessor& proc, double seconds)
    {
        juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        const int blocks = juce::jmax (1, juce::roundToInt (seconds * 44100.0 / 512.0));
        for (int b = 0; b < blocks; ++b)
        {
            midi.clear();
            proc.processBlock (buffer, midi);
            peak = juce::jmax (peak, buffer.getMagnitude (0, 0, 512));
        }
        return peak;
    }

    void waitForEditToSettle() { juce::Thread::sleep (260); }
}

TEST_CASE ("Audition holds a normal pad for the length of its sample",
           "[plugin][audition]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    const auto wav = writeSine (testDir ("audition_hold").getChildFile ("long.wav"), 1.0);
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);

    proc.auditionPad (0);
    renderPeak (proc, 0.5);
    // Halfway through a one-second sample the note must still be sounding,
    // not released at the end of the first block.
    REQUIRE (renderPeak (proc, 0.05) > 0.1f);
}

TEST_CASE ("Editing Start auditions the pad once the edit settles",
           "[plugin][audition][editaudition]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    const auto wav = writeSine (testDir ("edit_audition").getChildFile ("tone.wav"), 1.0);
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    proc.processEditAuditions();   // clear anything the load queued

    setParam (proc, param_ids::padStart (1), 0.2f);
    proc.processEditAuditions();             // too soon: still settling
    REQUIRE (renderPeak (proc, 0.05) < 1.0e-4f);

    waitForEditToSettle();
    proc.processEditAuditions();
    REQUIRE (renderPeak (proc, 0.05) > 0.1f);
}

TEST_CASE ("Audition On Edit can be switched off", "[plugin][audition][editaudition]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    const auto wav = writeSine (testDir ("edit_audition_off").getChildFile ("tone.wav"), 1.0);
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    setParam (proc, param_ids::auditionOnEdit, 0.0f);

    setParam (proc, param_ids::padEnd (1), 0.6f);
    waitForEditToSettle();
    proc.processEditAuditions();
    REQUIRE (renderPeak (proc, 0.05) < 1.0e-4f);
}

TEST_CASE ("Edits on several pads at once do not audition", "[plugin][audition][editaudition]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    const auto wav = writeSine (testDir ("edit_audition_batch").getChildFile ("tone.wav"), 1.0);
    REQUIRE (proc.loadSampleForPad (0, wav) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPad (1, wav) == ASamplerProcessor::LoadResult::Success);

    // What a chop or kit load looks like: several pads' windows rewritten.
    setParam (proc, param_ids::padStart (1), 0.1f);
    setParam (proc, param_ids::padStart (2), 0.3f);
    waitForEditToSettle();
    proc.processEditAuditions();
    REQUIRE (renderPeak (proc, 0.05) < 1.0e-4f);
}

// ----- Parameter groups and names -------------------------------------------

TEST_CASE ("Per-pad parameters are nested by section and carry the pad number",
           "[plugin][accessibility][groups]")
{
    ASamplerProcessor proc;

    const juce::AudioProcessorParameterGroup* pad9 = nullptr;
    for (auto* g : proc.getParameterTree().getSubgroups (false))
        if (g->getID() == "pad_9")
            pad9 = g;
    REQUIRE (pad9 != nullptr);
    REQUIRE (pad9->getName() == "Pad 9");

    const juce::AudioProcessorParameterGroup* filter = nullptr;
    for (auto* g : pad9->getSubgroups (false))
        if (g->getName() == "Filter")
            filter = g;
    REQUIRE (filter != nullptr);

    juce::StringArray names;
    for (auto* p : filter->getParameters (false))
        names.add (p->getName (128));
    REQUIRE (names.contains ("P9 Filter Cutoff"));
    REQUIRE (names.contains ("P9 Filter Env Release"));

    // The ID is unchanged by the regrouping, so saved projects still load.
    REQUIRE (proc.getState().getParameter (param_ids::padFilterCutoff (9))->getName (128)
             == "P9 Filter Cutoff");
}

TEST_CASE ("Focus proxies have stable IDs and Focus-prefixed names",
           "[plugin][accessibility][groups][focus]")
{
    ASamplerProcessor proc;
    auto* cutoff = findFocusProxy (proc, "Filter Cutoff");
    REQUIRE (cutoff != nullptr);
    REQUIRE (cutoff->getName (128) == "Focus Filter Cutoff");

    auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (cutoff);
    REQUIRE (withId != nullptr);
    REQUIRE (withId->getParameterID()
             == "focus_" + param_ids::padFilterCutoff (1).fromFirstOccurrenceOf ("pad_1_", false, false));
}

TEST_CASE ("Focus slice points read in the focused pad's sample",
           "[plugin][accessibility][slicetime][focus]")
{
    ASamplerProcessor proc;
    const auto dir = testDir ("focus_slice");
    REQUIRE (proc.loadSampleForPad (0, writeSine (dir.getChildFile ("one.wav"), 1.0))
             == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPad (1, writeSine (dir.getChildFile ("four.wav"), 4.0))
             == ASamplerProcessor::LoadResult::Success);

    auto* start = findFocusProxy (proc, "Start");
    REQUIRE (start != nullptr);
    REQUIRE (start->getText (0.5f, 64) == "500.0 ms");   // Pad 1: 1 s

    setParam (proc, param_ids::focusedPad, 2.0f);
    REQUIRE (start->getText (0.5f, 64) == "2.000 s");    // Pad 2: 4 s
}

// ----- Programs -------------------------------------------------------------

TEST_CASE ("Factory kits are exposed as host programs after a no-op Current",
           "[plugin][programs]")
{
    ASamplerProcessor proc;
    const auto kits = ASamplerProcessor::getFactoryKitNames();
    REQUIRE (proc.getNumPrograms() == kits.size() + 1);
    REQUIRE (proc.getProgramName (0) == "Current");
    for (int i = 0; i < kits.size(); ++i)
        REQUIRE (proc.getProgramName (i + 1) == kits[i]);
    REQUIRE (proc.getCurrentProgram() == 0);
}

TEST_CASE ("Selecting a program loads its kit; re-selecting it does not",
           "[plugin][programs]")
{
    juce::ScopedJuceInitialiser_GUI gui;   // so this thread is the message thread
    ASamplerProcessor proc;

    proc.setCurrentProgram (1);   // first factory kit
    REQUIRE (proc.getCurrentProgram() == 1);
    REQUIRE (proc.getLastStatusMessage().contains (ASamplerProcessor::getFactoryKitNames()[0]));

    // The user tweaks a parameter; the host then re-sends the same program.
    setParam (proc, param_ids::padVolume (1), -20.0f);
    proc.setCurrentProgram (1);
    REQUIRE (getParam (proc, param_ids::padVolume (1)) == Approx (-20.0f).margin (0.05f));

    // "Current" never changes anything.
    proc.setCurrentProgram (0);
    REQUIRE (getParam (proc, param_ids::padVolume (1)) == Approx (-20.0f).margin (0.05f));
}

TEST_CASE ("The selected program survives a state round-trip without reloading",
           "[plugin][programs][state]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        a.setCurrentProgram (2);
        setParam (a, param_ids::padVolume (3), -12.0f);
        a.getStateInformation (data);
    }

    ASamplerProcessor b;
    b.setStateInformation (data.getData(), (int) data.getSize());
    REQUIRE (b.getCurrentProgram() == 2);

    // A host re-selecting program 2 after the restore keeps the user's edit.
    b.setCurrentProgram (2);
    REQUIRE (getParam (b, param_ids::padVolume (3)) == Approx (-12.0f).margin (0.05f));
}

// ----- Polyphony ------------------------------------------------------------

TEST_CASE ("Every pad can hold eight voices at once", "[plugin][voices]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    const auto wav = writeSine (testDir ("poly").getChildFile ("long.wav"), 3.0);
    for (int p = 0; p < kNumPads; ++p)
    {
        REQUIRE (proc.loadSampleForPad (p, wav) == ASamplerProcessor::LoadResult::Success);
        setParam (proc, param_ids::padOneShot (p + 1), 1.0f);
    }

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    for (int hit = 0; hit < 8; ++hit)
    {
        juce::MidiBuffer midi;
        for (int p = 0; p < kNumPads; ++p)
            midi.addEvent (juce::MidiMessage::noteOn (1, 36 + p, 0.8f), hit * 8);
        proc.processBlock (buffer, midi);
    }

    REQUIRE (proc.getActiveVoiceCount() == kNumPads * 8);
}

// Not part of the suite (hidden tag): prints the render cost of a full pool.
// Run with: ASamplerTests.exe "[.benchmark]"
TEST_CASE ("Render cost with every voice sounding", "[.benchmark]")
{
    // Once at the original pitch (cubic reads) and once an octave up, where
    // every voice reads through the band-limited kernel — the worst case.
    for (const float pitch : { 0.0f, 12.0f })
    {
        ASamplerProcessor proc;
        proc.prepareToPlay (44100.0, 512);
        const auto wav = writeSine (testDir ("bench").getChildFile ("long.wav"), 30.0);
        for (int p = 0; p < kNumPads; ++p)
        {
            REQUIRE (proc.loadSampleForPad (p, wav) == ASamplerProcessor::LoadResult::Success);
            setParam (proc, param_ids::padOneShot (p + 1), 1.0f);
            setParam (proc, param_ids::padFilterMode (p + 1), 1.0f);   // low-pass on: realistic cost
            setParam (proc, param_ids::padPitch (p + 1), pitch);
        }

        juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
        for (int hit = 0; hit < kNumVoices / kNumPads; ++hit)
        {
            juce::MidiBuffer midi;
            for (int p = 0; p < kNumPads; ++p)
                midi.addEvent (juce::MidiMessage::noteOn (1, 36 + p, 0.8f), 0);
            proc.processBlock (buffer, midi);
        }
        REQUIRE (proc.getActiveVoiceCount() == kNumVoices);

        constexpr int blocks = 431;   // ~5 s of audio
        juce::MidiBuffer none;
        const auto t0 = juce::Time::getHighResolutionTicks();
        for (int b = 0; b < blocks; ++b)
            proc.processBlock (buffer, none);
        const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);

        const double audioSecs = blocks * 512 / 44100.0;
        WARN (kNumVoices << " voices at +" << pitch << " st: "
              << secs * 1000.0 / blocks << " ms per 512-sample block, "
              << 100.0 * secs / audioSecs << "% of one core in real time");
    }
}

// ----- Bounce ---------------------------------------------------------------

TEST_CASE ("Bouncing a pad renders its processing to a WAV on the next empty pad",
           "[plugin][bounce]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    const auto dir = testDir ("bounce");
    const auto wav = writeSine (dir.getChildFile ("tone.wav"), 1.0, 440.0f, 0.5f);
    REQUIRE (proc.loadSampleForPad (2, wav) == ASamplerProcessor::LoadResult::Success);

    // A short decay and no sustain: the bounce must carry the envelope.
    setParam (proc, param_ids::padSustain (3), 0.0f);
    setParam (proc, param_ids::padDecay (3),   0.1f);
    setParam (proc, param_ids::padVolume (3),  -6.0f);

    const auto r = proc.bouncePad (2, dir.getChildFile ("out"));
    REQUIRE (r.ok);
    REQUIRE (r.file.existsAsFile());
    REQUIRE (r.targetPad == 3);   // next empty pad after Pad 3
    REQUIRE (proc.getPadSampleDisplayName (3) == r.file.getFileName());
    REQUIRE (proc.getLastStatusMessage().startsWith ("Bounced Pad 3 to Pad 4"));

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (r.file));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == 2);

    // The envelope decayed well before the 1 s source ended, and the silent
    // tail was trimmed.
    const double seconds = reader->lengthInSamples / reader->sampleRate;
    REQUIRE (seconds < 0.8);

    juce::AudioBuffer<float> audio (2, (int) reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    // -6 dB volume on a 0.5 sine (and centre pan) keeps it well under 0.5.
    REQUIRE (audio.getMagnitude (0, 0, audio.getNumSamples()) < 0.4f);
    REQUIRE (audio.getMagnitude (0, 0, audio.getNumSamples()) > 0.05f);
}

TEST_CASE ("Bouncing with no empty pad still saves the file", "[plugin][bounce]")
{
    ASamplerProcessor proc;
    const auto dir = testDir ("bounce_full");
    const auto wav = writeSine (dir.getChildFile ("tone.wav"), 0.2);
    for (int p = 0; p < kNumPads; ++p)
        REQUIRE (proc.loadSampleForPad (p, wav) == ASamplerProcessor::LoadResult::Success);

    const auto r = proc.bouncePad (0, dir.getChildFile ("out"));
    REQUIRE (r.ok);
    REQUIRE (r.file.existsAsFile());
    REQUIRE (r.targetPad == -1);
    REQUIRE (proc.getLastStatusMessage().contains ("No empty pad"));
}

// ----- Robustness: damaged state --------------------------------------------

namespace
{
    // After loading damaged state the processor must still produce finite
    // audio and hold finite parameter values.
    void requireHealthy (ASamplerProcessor& proc)
    {
        for (auto* p : proc.getParameters())
            REQUIRE (std::isfinite (p->getValue()));

        proc.prepareToPlay (44100.0, 512);
        juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        proc.processBlock (buffer, midi);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                REQUIRE (std::isfinite (buffer.getSample (ch, i)));
    }

    void loadXml (ASamplerProcessor& proc, const juce::String& xmlText)
    {
        juce::MemoryBlock block;
        if (auto xml = juce::parseXML (xmlText))
            juce::AudioProcessor::copyXmlToBinary (*xml, block);
        else
            block.append (xmlText.toRawUTF8(), xmlText.getNumBytesAsUTF8());
        proc.setStateInformation (block.getData(), (int) block.getSize());
    }
}

TEST_CASE ("Random bytes as state are ignored without crashing", "[plugin][state][robustness]")
{
    juce::Random rng (1234);
    for (int size : { 0, 1, 7, 8, 64, 4096 })
    {
        CAPTURE (size);
        juce::MemoryBlock junk ((size_t) size);
        for (int i = 0; i < size; ++i)
            static_cast<char*> (junk.getData())[i] = (char) rng.nextInt (256);

        ASamplerProcessor proc;
        proc.setStateInformation (junk.getData(), size);
        requireHealthy (proc);
    }
}

TEST_CASE ("A truncated project chunk is ignored without crashing", "[plugin][state][robustness]")
{
    juce::MemoryBlock good;
    {
        ASamplerProcessor a;
        setParam (a, param_ids::padVolume (1), -9.0f);
        a.getStateInformation (good);
    }

    for (size_t cut : { good.getSize() / 4, good.getSize() / 2, good.getSize() - 3 })
    {
        CAPTURE (cut);
        ASamplerProcessor proc;
        proc.setStateInformation (good.getData(), (int) cut);
        requireHealthy (proc);
    }
}

TEST_CASE ("State from something else keeps the current settings", "[plugin][state][robustness]")
{
    ASamplerProcessor proc;
    setParam (proc, param_ids::padVolume (2), -15.0f);
    loadXml (proc, "<SomeOtherPlugin><PARAM id=\"pad_2_volume\" value=\"6\"/></SomeOtherPlugin>");
    REQUIRE (getParam (proc, param_ids::padVolume (2)) == Approx (-15.0f).margin (0.05f));
    REQUIRE (proc.getLastStatusMessage().contains ("Could not read"));
    requireHealthy (proc);
}

TEST_CASE ("Non-finite and out-of-range values in state are made safe",
           "[plugin][state][robustness]")
{
    ASamplerProcessor proc;
    loadXml (proc,
        "<STATE>"
        "  <PARAM id=\"pad_1_volume\" value=\"nan\"/>"
        "  <PARAM id=\"pad_1_pitch\" value=\"inf\"/>"
        "  <PARAM id=\"pad_1_cutoff\" value=\"1e30\"/>"
        "  <PARAM id=\"no_such_param\" value=\"3\"/>"
        "  <samples>"
        "    <sample index=\"-4\" layer=\"0\" path=\"x.wav\"/>"
        "    <sample index=\"99\" layer=\"0\" path=\"x.wav\"/>"
        "    <sample index=\"0\" layer=\"77\" path=\"x.wav\"/>"
        "    <sample index=\"1\" layer=\"0\"/>"
        "    <sample index=\"2\" layer=\"0\" path=\"\u0001\u0002 not a path\"/>"
        "  </samples>"
        "  <midilearn>"
        "    <cc number=\"500\" paramId=\"pad_1_volume\"/>"
        "    <cc number=\"5\" paramId=\"missing_param\"/>"
        "  </midilearn>"
        "</STATE>");
    requireHealthy (proc);

    // A malformed sample entry is kept as a missing reference at most, never
    // applied to a pad or layer that does not exist.
    REQUIRE (proc.getMissingSampleCount() <= 1);
}

TEST_CASE ("Restoring state clears the undo history", "[plugin][state][undo]")
{
    juce::MemoryBlock data;
    {
        ASamplerProcessor a;
        a.getStateInformation (data);
    }

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPad (0, writeSine (testDir ("undo_restore").getChildFile ("t.wav"), 1.0))
             == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.chopAcrossPads (0) == ASamplerProcessor::ChopResult::Success);
    REQUIRE (proc.getUndoManager().canUndo());
    proc.setStateInformation (data.getData(), (int) data.getSize());
    REQUIRE_FALSE (proc.getUndoManager().canUndo());
}

// ----- Documentation helper --------------------------------------------------
//
// Hidden: writes every global, Focus and Pad 1 parameter with its range and
// default, as the host would display them, to ASAMPLER_PARAM_DUMP (or the temp
// folder). Used to keep the manual's parameter reference true to the code:
//   ASamplerTests.exe "[.paramdump]"

TEST_CASE ("Dump parameter reference", "[.paramdump]")
{
    ASamplerProcessor proc;
    auto target = juce::SystemStats::getEnvironmentVariable ("ASAMPLER_PARAM_DUMP", {});
    juce::File file = target.isNotEmpty()
                        ? juce::File (target)
                        : juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("asampler_params.tsv");

    juce::String out = "id\tname\tgroup\tmin\tmax\tdefault\tsteps\n";
    for (auto* p : proc.getParameters())
    {
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p);
        const auto id = withId != nullptr ? withId->paramID : juce::String();
        if (id.startsWith ("pad_") && ! id.startsWith ("pad_1_"))
            continue;

        juce::String group;
        if (auto* g = proc.getParameterTree().getGroupsForParameter (p).getLast())
            group = g->getName();

        out << id << '\t' << p->getName (100) << '\t' << group << '\t'
            << p->getText (0.0f, 100) << '\t' << p->getText (1.0f, 100) << '\t'
            << p->getText (p->getDefaultValue(), 100) << '\t' << p->getNumSteps() << '\n';
    }

    REQUIRE (file.replaceWithText (out));
    WARN ("Wrote " << file.getFullPathName());
}
