// Workflow features: pad names, applying a setting to many pads, note learn,
// loading many files at once, kits that carry their samples (or none),
// silence trimming, slice-point scrubbing, try-before-you-keep sample preview,
// computer-key notes sent as MIDI, and the REAPER hooks (with a fake REAPER).

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
        auto* p = proc.getState().getParameter (id);
        REQUIRE (p != nullptr);
        return p->convertFrom0to1 (p->getValue());
    }

    // A fresh, empty folder under temp for one test.
    juce::File freshDir (const juce::String& name)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("asampler_workflow_" + name);
        dir.deleteRecursively();
        dir.createDirectory();
        return dir;
    }

    juce::File writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer)
    {
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

    juce::File writeConstant (const juce::File& file, float amplitude, int numSamples = 8192)
    {
        juce::AudioBuffer<float> b (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            b.setSample (0, i, amplitude);
        return writeWav (file, b);
    }

    void makeFlat (ASamplerProcessor& proc, int padOneBased)
    {
        setParam (proc, param_ids::padVelocityCurve (padOneBased), static_cast<float> (VelocityCurve::Fixed));
        setParam (proc, param_ids::padAttack  (padOneBased), 0.0f);
        setParam (proc, param_ids::padDecay   (padOneBased), 0.0f);
        setParam (proc, param_ids::padSustain (padOneBased), 1.0f);
        setParam (proc, param_ids::padRelease (padOneBased), 0.0f);
        setParam (proc, param_ids::padPan     (padOneBased), 0.0f);
    }

    float renderPeak (ASamplerProcessor& proc, juce::MidiBuffer midi, int numSamples = 512)
    {
        juce::AudioBuffer<float> buf (2, numSamples);
        proc.processBlock (buf, midi);
        return buf.getMagnitude (0, 0, numSamples);
    }

    float hit (ASamplerProcessor& proc, int note)
    {
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
        const float peak = renderPeak (proc, on);
        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        renderPeak (proc, off);
        renderPeak (proc, {});
        return peak;
    }

    float stateOf (ASamplerProcessor& proc, juce::MemoryBlock& out)
    {
        proc.getStateInformation (out);
        return (float) out.getSize();
    }
}

TEST_CASE ("Pad names are saved with the project, copied with the pad and cleared by Clear All",
           "[workflow][names][plugin]")
{
    ASamplerProcessor proc;
    proc.setPadName (2, "  Snare 2  ");
    CHECK (proc.getPadName (2) == "Snare 2");
    CHECK (proc.getLastStatusMessage() == "Pad 3 named Snare 2");

    juce::MemoryBlock saved;
    stateOf (proc, saved);

    ASamplerProcessor restored;
    restored.setStateInformation (saved.getData(), (int) saved.getSize());
    CHECK (restored.getPadName (2) == "Snare 2");
    CHECK (restored.getPadName (0).isEmpty());

    restored.copyPad (2);
    restored.pastePad (7, true);
    CHECK (restored.getPadName (7) == "Snare 2");

    restored.clearAllPads();
    CHECK (restored.getPadName (2).isEmpty());
    CHECK (restored.getPadName (7).isEmpty());
}

TEST_CASE ("A setting applied to all pads changes every pad and undoes as one step",
           "[workflow][apply][undo][plugin]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    int cutoffIndex = -1;
    for (int i = 0; i < ASamplerProcessor::getNumPadControls(); ++i)
        if (ASamplerProcessor::getPadControlName (i) == "Filter Cutoff") cutoffIndex = i;
    REQUIRE (cutoffIndex >= 0);
    CHECK (ASamplerProcessor::getPadControlSection (cutoffIndex) == "Filter");

    const float original = getParam (proc, param_ids::padFilterCutoff (2));
    setParam (proc, param_ids::padFilterCutoff (1), 1200.0f);
    const float set = getParam (proc, param_ids::padFilterCutoff (1));

    const int changed = proc.applyControlToPads (0, cutoffIndex, 0, kNumPads - 1);
    CHECK (changed == kNumPads - 1);
    for (int p = 1; p <= kNumPads; ++p)
        CHECK (getParam (proc, param_ids::padFilterCutoff (p)) == Approx (set));
    CHECK (proc.getLastStatusMessage().contains ("applied to Pads 1 to 32"));

    REQUIRE (proc.undoLastAction());
    CHECK (getParam (proc, param_ids::padFilterCutoff (2)) == Approx (original));
    CHECK (getParam (proc, param_ids::padFilterCutoff (32)) == Approx (original));
    CHECK (getParam (proc, param_ids::padFilterCutoff (1)) == Approx (set));   // the source stays

    SECTION ("a whole section, limited to one bank")
    {
        setParam (proc, param_ids::padAttack (1), 0.25f);
        setParam (proc, param_ids::padRelease (1), 1.5f);
        const float attack = getParam (proc, param_ids::padAttack (1));
        CHECK (proc.applySectionToPads (0, "Envelope", 0, 15) == 15);
        CHECK (getParam (proc, param_ids::padAttack (16)) == Approx (attack));
        CHECK (getParam (proc, param_ids::padRelease (9)) == Approx (getParam (proc, param_ids::padRelease (1))));
        CHECK (getParam (proc, param_ids::padAttack (17)) != Approx (attack));
    }
}

TEST_CASE ("Note learn takes the next note played, swallows it, and sets the pad's note",
           "[workflow][notelearn][plugin]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    makeFlat (proc, 2);

    const int pad2Note = juce::roundToInt (getParam (proc, param_ids::padNote (2)));

    proc.armNoteLearn (0, ASamplerProcessor::NoteLearnTarget::Note);
    CHECK (proc.isNoteLearnArmed());

    // Pad 2's note: it would sound, but while learning it is consumed.
    CHECK (hit (proc, pad2Note) == 0.0f);

    proc.processPendingNoteLearn();
    CHECK_FALSE (proc.isNoteLearnArmed());
    CHECK (juce::roundToInt (getParam (proc, param_ids::padNote (1))) == pad2Note);
    CHECK (proc.getLastStatusMessage().startsWith ("Pad 1 Note set to"));

    // Disarmed: notes play again.
    CHECK (hit (proc, pad2Note) > 0.0f);

    SECTION ("Low and High keep the range the right way up")
    {
        setParam (proc, param_ids::padLowNote (1), 40.0f);
        setParam (proc, param_ids::padHighNote (1), 50.0f);
        proc.armNoteLearn (0, ASamplerProcessor::NoteLearnTarget::LowNote);
        hit (proc, 60);
        proc.processPendingNoteLearn();
        CHECK (juce::roundToInt (getParam (proc, param_ids::padLowNote (1))) == 60);
        CHECK (juce::roundToInt (getParam (proc, param_ids::padHighNote (1))) == 60);
    }

    SECTION ("Cancel leaves everything as it was")
    {
        proc.armNoteLearn (0, ASamplerProcessor::NoteLearnTarget::HighNote);
        proc.cancelNoteLearn();
        CHECK_FALSE (proc.isNoteLearnArmed());
        CHECK (hit (proc, pad2Note) > 0.0f);
    }
}

TEST_CASE ("Many files load onto consecutive pads, or as round-robin or velocity layers",
           "[workflow][multiload][plugin]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    const auto dir = freshDir ("multiload");
    juce::Array<juce::File> files { writeConstant (dir.getChildFile ("hit 10.wav"), 0.3f),
                                    writeConstant (dir.getChildFile ("hit 2.wav"), 0.2f),
                                    writeConstant (dir.getChildFile ("hit 1.wav"), 0.1f) };

    SECTION ("one per pad, in natural order")
    {
        CHECK (proc.loadSamples (4, files, ASamplerProcessor::MultiLoadMode::OnePerPad, true) == 3);
        CHECK (proc.getPadLayerSampleDisplayName (4, 0) == "hit 1.wav");
        CHECK (proc.getPadLayerSampleDisplayName (5, 0) == "hit 2.wav");
        CHECK (proc.getPadLayerSampleDisplayName (6, 0) == "hit 10.wav");
        CHECK (proc.getLastStatusMessage() == "Loaded 3 samples onto Pads 5 to 7");
    }

    SECTION ("files beyond the last pad are skipped and said so")
    {
        CHECK (proc.loadSamples (kNumPads - 2, files, ASamplerProcessor::MultiLoadMode::OnePerPad, true) == 2);
        CHECK (proc.getLastStatusMessage().contains ("1 more did not fit"));
    }

    SECTION ("round robin replaces the pad's layers")
    {
        proc.loadSampleForPadLayer (0, 5, files[0]);   // a leftover on layer 6
        proc.loadSamples (0, files, ASamplerProcessor::MultiLoadMode::RoundRobin, true);
        CHECK (juce::roundToInt (getParam (proc, param_ids::padLayerMode (1))) == (int) LayerMode::RoundRobin);
        CHECK (proc.getPadLayerSampleDisplayName (0, 2) == "hit 10.wav");
        CHECK (proc.getPadLayerSampleDisplayName (0, 5) == "default");
    }

    SECTION ("velocity layers get evenly spread ranges")
    {
        proc.loadSamples (0, files, ASamplerProcessor::MultiLoadMode::VelocityLayers, true);
        CHECK (juce::roundToInt (getParam (proc, param_ids::padLayerMode (1))) == (int) LayerMode::Velocity);
        CHECK (juce::roundToInt (getParam (proc, param_ids::padLayerVelLow (1, 1))) == 0);
        CHECK (juce::roundToInt (getParam (proc, param_ids::padLayerVelHigh (1, 1))) == 42);
        CHECK (juce::roundToInt (getParam (proc, param_ids::padLayerVelLow (1, 2))) == 43);
        CHECK (juce::roundToInt (getParam (proc, param_ids::padLayerVelHigh (1, 2))) == 84);
        CHECK (juce::roundToInt (getParam (proc, param_ids::padLayerVelLow (1, 3))) == 85);
        CHECK (juce::roundToInt (getParam (proc, param_ids::padLayerVelHigh (1, 3))) == 127);

    }
}

TEST_CASE ("A kit saved with its samples loads from a moved folder", "[workflow][kits][plugin]")
{
    const auto dirA = freshDir ("kitA");
    const auto source = writeConstant (dirA.getChildFile ("my kick.wav"), 0.4f);

    {
        ASamplerProcessor proc;
        REQUIRE (proc.loadSampleForPadLayer (0, 0, source) == ASamplerProcessor::LoadResult::Success);
        proc.loadSampleForPadLayer (3, 0, source);   // same file twice: copied once
        proc.setPadName (0, "Kick");
        REQUIRE (proc.saveKitToFile (dirA.getChildFile ("Beat.asamplerkit"),
                                     ASamplerProcessor::KitSaveMode::WithSamples));
        CHECK (proc.getLastStatusMessage().contains ("with 1 sample"));
    }
    CHECK (dirA.getChildFile ("Beat samples").getChildFile ("my kick.wav").existsAsFile());

    // Move the kit and its folder elsewhere, and remove the originals.
    const auto dirB = freshDir ("kitB");
    REQUIRE (dirA.getChildFile ("Beat.asamplerkit").copyFileTo (dirB.getChildFile ("Beat.asamplerkit")));
    REQUIRE (dirA.getChildFile ("Beat samples").copyDirectoryTo (dirB.getChildFile ("Beat samples")));
    dirA.deleteRecursively();

    ASamplerProcessor proc;
    REQUIRE (proc.loadKitFromFile (dirB.getChildFile ("Beat.asamplerkit")));
    CHECK (proc.getMissingSampleCount() == 0);
    CHECK (proc.getPadLayerSampleDisplayName (0, 0) == "my kick.wav");
    CHECK (proc.getPadLayerSampleDisplayName (3, 0) == "my kick.wav");
    CHECK (proc.getPadName (0) == "Kick");
}

TEST_CASE ("A kit template restores settings and keeps the current samples", "[workflow][kits][plugin]")
{
    const auto dir = freshDir ("template");
    const auto a = writeConstant (dir.getChildFile ("a.wav"), 0.4f);

    ASamplerProcessor proc;
    setParam (proc, param_ids::padAttack (1), 0.5f);
    const float attack = getParam (proc, param_ids::padAttack (1));
    REQUIRE (proc.saveKitToFile (dir.getChildFile ("T.asamplerkit"), ASamplerProcessor::KitSaveMode::Template));
    CHECK (proc.getLastStatusMessage().contains ("settings only"));

    setParam (proc, param_ids::padAttack (1), 0.0f);
    proc.loadSampleForPadLayer (0, 0, a);
    proc.setPadName (0, "Mine");

    REQUIRE (proc.loadKitFromFile (dir.getChildFile ("T.asamplerkit")));
    CHECK (getParam (proc, param_ids::padAttack (1)) == Approx (attack));
    CHECK (proc.getPadLayerSampleDisplayName (0, 0) == "a.wav");
    CHECK (proc.getPadName (0) == "Mine");
}

TEST_CASE ("Trim silence moves Start to the onset and End to the tail", "[workflow][trim][plugin]")
{
    const auto dir = freshDir ("trim");
    // 0.5 s of silence, 0.5 s of tone, 1 s of silence.
    juce::AudioBuffer<float> b (1, 88200);
    b.clear();
    for (int i = 22050; i < 44100; ++i)
        b.setSample (0, i, 0.5f * std::sin (0.05f * (float) i));
    const auto file = writeWav (dir.getChildFile ("gap.wav"), b);

    ASamplerProcessor proc;
    REQUIRE (proc.loadSampleForPadLayer (0, 0, file) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.trimSilence (0));

    const float start = getParam (proc, param_ids::padStart (1));
    const float end   = getParam (proc, param_ids::padEnd (1));
    CHECK (start * 2.0f == Approx (0.5f - 0.001f).margin (0.002f));
    CHECK (end * 2.0f   == Approx (1.0f + 0.005f).margin (0.002f));
    CHECK (proc.getLastStatusMessage().startsWith ("Trimmed Pad 1: Start"));

    REQUIRE (proc.undoLastAction());
    CHECK (getParam (proc, param_ids::padStart (1)) == Approx (0.0f));

    SECTION ("on load, when the global switch is on")
    {
        setParam (proc, param_ids::trimOnLoad, 1.0f);
        REQUIRE (proc.loadSampleForPadLayer (1, 0, file) == ASamplerProcessor::LoadResult::Success);
        CHECK (getParam (proc, param_ids::padStart (2)) * 2.0f == Approx (0.499f).margin (0.002f));
    }
}

TEST_CASE ("Slice scrubbing plays a snippet forward from Start and backwards into End",
           "[workflow][scrub][plugin]")
{
    const auto dir = freshDir ("scrub");
    // A rising ramp: level tells position.
    juce::AudioBuffer<float> b (1, 44100);
    for (int i = 0; i < 44100; ++i)
        b.setSample (0, i, (float) i / 44100.0f);
    const auto file = writeWav (dir.getChildFile ("ramp.wav"), b);

    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    setParam (proc, param_ids::masterGain, 0.0f);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, file) == ASamplerProcessor::LoadResult::Success);

    auto render = [&proc] (int n)
    {
        juce::AudioBuffer<float> buf (2, n);
        juce::MidiBuffer midi;
        proc.processBlock (buf, midi);
        return buf;
    };

    SECTION ("Start: forward from the point, then stops")
    {
        proc.previewSlicePoint (0, false, 0.5f);
        CHECK (proc.isPreviewPlaying());
        auto buf = render (512);
        CHECK (buf.getSample (0, 200) == Approx (0.5f + 200.0f / 44100.0f).margin (0.003f));
        CHECK (buf.getSample (0, 400) > buf.getSample (0, 200));
        for (int i = 0; i < 10; ++i) render (512);
        CHECK_FALSE (proc.isPreviewPlaying());
    }

    SECTION ("End: backwards from the point")
    {
        proc.previewSlicePoint (0, true, 0.5f);
        auto buf = render (512);
        CHECK (buf.getSample (0, 200) == Approx (0.5f - 201.0f / 44100.0f).margin (0.003f));
        CHECK (buf.getSample (0, 400) < buf.getSample (0, 200));
    }

    SECTION ("confirming sets the parameter as one undo step")
    {
        proc.setSlicePoint (0, false, 0.25f);
        CHECK (proc.getSlicePoint (0, false) == Approx (0.25f).margin (1.0e-4));
        CHECK (proc.getLastStatusMessage() == "Pad 1 Start 250.0 ms");
        REQUIRE (proc.undoLastAction());
        CHECK (proc.getSlicePoint (0, false) == Approx (0.0f));
    }
}

TEST_CASE ("Sample preview tries files on the pad without replacing its sample",
           "[workflow][preview][plugin]")
{
    const auto dir = freshDir ("preview");
    const auto a = writeConstant (dir.getChildFile ("kick a.wav"), 0.1f);
    const auto b = writeConstant (dir.getChildFile ("kick b.wav"), 0.2f);
    writeConstant (dir.getChildFile ("kick c.wav"), 0.3f);

    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    setParam (proc, param_ids::masterGain, 0.0f);
    makeFlat (proc, 1);
    setParam (proc, param_ids::padVolume (1), 0.0f);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, a) == ASamplerProcessor::LoadResult::Success);
    const int note = juce::roundToInt (getParam (proc, param_ids::padNote (1)));
    const float levelA = hit (proc, note);
    REQUIRE (levelA > 0.0f);

    REQUIRE (proc.previewSample (0, +1, true) == ASamplerProcessor::BrowseResult::Success);
    CHECK (proc.isPreviewing (0));
    CHECK (proc.getPreviewFile (0) == b);
    CHECK (proc.getPadLayerSampleDisplayName (0, 0) == "kick a.wav");
    CHECK (proc.getLastStatusMessage() == "Trying kick b, 2 of 3. Enter keeps it, Escape goes back");
    renderPeak (proc, {}, 16384);   // let the automatic audition finish

    CHECK (hit (proc, note) == Approx (levelA * 2.0f).margin (0.01f));

    proc.previewSample (0, +1, true);
    CHECK (proc.getLastStatusMessage() == "Trying kick c, 3 of 3");
    proc.previewSample (0, -1, true);
    CHECK (proc.getPreviewFile (0) == b);
    renderPeak (proc, {}, 16384);

    SECTION ("Escape goes back")
    {
        proc.cancelPreview (0);
        CHECK_FALSE (proc.isPreviewing (0));
        CHECK (proc.getLastStatusMessage() == "Back to kick a.wav on Pad 1");
        CHECK (hit (proc, note) == Approx (levelA).margin (0.01f));
    }

    SECTION ("Enter keeps it")
    {
        REQUIRE (proc.keepPreview (0));
        CHECK_FALSE (proc.isPreviewing (0));
        CHECK (proc.getPadLayerSampleDisplayName (0, 0) == "kick b.wav");
        CHECK (proc.getLastStatusMessage() == "Kept kick b on Pad 1");
        renderPeak (proc, {}, 16384);
        CHECK (hit (proc, note) == Approx (levelA * 2.0f).margin (0.01f));
    }

    SECTION ("an empty pad starts in the last folder used")
    {
        REQUIRE (proc.previewSample (5, +1, true) == ASamplerProcessor::BrowseResult::Success);
        CHECK (proc.getPreviewFile (5) == a);
    }
}

TEST_CASE ("Computer-key notes play the pad and go out as MIDI at the key velocity",
           "[workflow][livenotes][plugin]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    CHECK (proc.producesMidi());
    setParam (proc, param_ids::keyVelocity, 90.0f);

    const int note = proc.getPadPlayNote (0);
    proc.queueLiveNote (note, true);

    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 7, 100), 0);   // host input is not echoed
    proc.processBlock (buf, midi);
    CHECK (buf.getMagnitude (0, 0, 512) > 0.0f);

    int ons = 0, others = 0;
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn() && m.getNoteNumber() == note && m.getVelocity() == 90) ++ons;
        else ++others;
    }
    CHECK (ons == 1);
    CHECK (others == 0);

    proc.queueLiveNote (note, false);
    juce::MidiBuffer next;
    proc.processBlock (buf, next);
    REQUIRE (next.getNumEvents() == 1);
    CHECK ((*next.begin()).getMessage().isNoteOff());
}

namespace fake_reaper
{
    int recordCalls = 0, undoCalls = 0, changeCount = 0, selected = 1, glueCommand = 0;
    juce::File glued;

    void onRecord() { ++recordCalls; }
    int  countSelected (void*) { return selected; }
    void* selectedItem (void*, int) { return &selected; }
    void mainOnCommand (int cmd, int)
    {
        glueCommand = cmd;
        writeConstant (glued, 0.25f);
        ++changeCount;
    }
    void* activeTake (void* item) { return item; }
    void* takeSource (void* take) { return take; }
    void sourceFileName (void*, char* buf, int size) { glued.getFullPathName().copyToUTF8 (buf, (size_t) size); }
    int  undo (void*) { ++undoCalls; --changeCount; return 1; }
    int  stateCount (void*) { return changeCount; }
}

TEST_CASE ("REAPER: record toggles and the selected item is grabbed onto a pad",
           "[workflow][reaper][plugin]")
{
    ASamplerProcessor proc;

    CHECK_FALSE (proc.toggleReaperRecord());
    CHECK (proc.getLastStatusMessage() == "Record works only when ASampler runs in REAPER");
    CHECK_FALSE (proc.grabReaperItem (0));

    const auto dir = freshDir ("reaper");
    fake_reaper::glued = dir.getChildFile ("glued-001.wav");
    fake_reaper::recordCalls = fake_reaper::undoCalls = fake_reaper::changeCount = 0;

    auto& api = proc.getReaperApi();
    api.csurfOnRecord              = fake_reaper::onRecord;
    api.countSelectedMediaItems    = fake_reaper::countSelected;
    api.getSelectedMediaItem       = fake_reaper::selectedItem;
    api.mainOnCommand              = fake_reaper::mainOnCommand;
    api.getActiveTake              = fake_reaper::activeTake;
    api.getMediaItemTakeSource     = fake_reaper::takeSource;
    api.getMediaSourceFileName     = fake_reaper::sourceFileName;
    api.undoDoUndo2                = fake_reaper::undo;
    api.getProjectStateChangeCount = fake_reaper::stateCount;

    CHECK (proc.toggleReaperRecord());
    CHECK (fake_reaper::recordCalls == 1);

    fake_reaper::selected = 2;
    CHECK_FALSE (proc.grabReaperItem (0, dir.getChildFile ("out")));
    CHECK (proc.getLastStatusMessage().contains ("2 are selected"));

    fake_reaper::selected = 1;
    REQUIRE (proc.grabReaperItem (2, dir.getChildFile ("out")));
    CHECK (fake_reaper::glueCommand == 40257);
    CHECK (fake_reaper::undoCalls == 1);
    CHECK (fake_reaper::changeCount == 0);
    CHECK (proc.getPadLayerSampleDisplayName (2, 0) == "Pad 3 glued-001.wav");
    CHECK (dir.getChildFile ("out").getChildFile ("Pad 3 glued-001.wav").existsAsFile());
    CHECK (proc.getLastStatusMessage() == "Grabbed the selected REAPER item onto Pad 3");
}
