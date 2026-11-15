// Tuning a pad to its sample's pitch, the Hold / Exit Loop On Release
// settings reaching the voices, and the REAPER undo / script hooks.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginProcessor.h"

#include <cmath>
#include <cstring>
#include <map>

using namespace asampler;

namespace
{
    juce::File freshDir (const juce::String& name)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("asampler_sampling_" + name);
        dir.deleteRecursively();
        dir.createDirectory();
        return dir;
    }

    juce::File writeWav (const juce::File& file, const juce::AudioBuffer<float>& b, double sr = 44100.0)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (new juce::FileOutputStream (file), sr, (unsigned) b.getNumChannels(), 24, {}, 0));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        return file;
    }

    juce::AudioBuffer<float> sine (double hz, int n, float amp = 0.5f, double sr = 44100.0)
    {
        juce::AudioBuffer<float> b (1, n);
        for (int i = 0; i < n; ++i)
            b.setSample (0, i, amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / sr));
        return b;
    }

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

    // One block; returns its output peak.
    float runBlock (ASamplerProcessor& proc, int n)
    {
        juce::AudioBuffer<float> buf (2, n);
        buf.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buf, midi);
        return buf.getMagnitude (0, n);
    }
}

TEST_CASE ("Tune sets Pitch from the sample's detected pitch", "[plugin][tune][undo]")
{
    ASamplerProcessor proc;
    const auto dir = freshDir ("tune");

    // An A 20 cents sharp, on a Fixed pad: pulled into tune.
    const double sharpA = 440.0 * std::pow (2.0, 0.2 / 12.0);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, writeWav (dir.getChildFile ("a.wav"), sine (sharpA, 44100)))
             == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.tunePad (0));
    REQUIRE (getParam (proc, param_ids::padPitch (1)) == Catch::Approx (-0.20f).margin (0.03f));
    REQUIRE (proc.getLastStatusMessage().contains ("in tune"));
    REQUIRE (proc.undoLastAction());
    REQUIRE (getParam (proc, param_ids::padPitch (1)) == 0.0f);

    // The same on a Keyboard pad rooted on C: an A moves up 3 semitones to C.
    setParam (proc, param_ids::padMode (1), (float) static_cast<int> (PadMode::Keyboard));
    REQUIRE (proc.tunePad (0));
    REQUIRE (getParam (proc, param_ids::padPitch (1)) == Catch::Approx (2.80f).margin (0.03f));
    REQUIRE (proc.getLastStatusMessage().contains ("on its root key"));

    // Noise has no pitch: nothing changes.
    juce::AudioBuffer<float> noise (1, 44100);
    juce::Random rng (3);
    for (int i = 0; i < noise.getNumSamples(); ++i)
        noise.setSample (0, i, rng.nextFloat() - 0.5f);
    REQUIRE (proc.loadSampleForPadLayer (1, 0, writeWav (dir.getChildFile ("n.wav"), noise))
             == ASamplerProcessor::LoadResult::Success);
    REQUIRE_FALSE (proc.tunePad (1));
    REQUIRE (getParam (proc, param_ids::padPitch (2)) == 0.0f);

    // The built-in tone is not something to tune.
    REQUIRE_FALSE (proc.tunePad (5));
}

TEST_CASE ("Hold reaches the voices", "[plugin][hold]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    const auto dir = freshDir ("hold");
    juce::AudioBuffer<float> dc (1, 44100);
    dc.clear();
    for (int i = 0; i < dc.getNumSamples(); ++i) dc.setSample (0, i, 0.5f);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, writeWav (dir.getChildFile ("dc.wav"), dc))
             == ASamplerProcessor::LoadResult::Success);

    setParam (proc, param_ids::padAttack (1), 0.0f);
    setParam (proc, param_ids::padDecay (1), 0.05f);
    setParam (proc, param_ids::padSustain (1), 0.0f);
    setParam (proc, param_ids::padVelocityCurve (1), (float) static_cast<int> (VelocityCurve::Fixed));

    auto levelAt150ms = [&]
    {
        proc.auditionPad (0, 1.0f, false);
        float last = 0.0f;
        for (int b = 0; b < 14; ++b)   // ~160 ms
            last = runBlock (proc, 512);
        for (int b = 0; b < 200; ++b)  // let it finish
            runBlock (proc, 512);
        return last;
    };

    REQUIRE (levelAt150ms() < 0.01f);
    setParam (proc, param_ids::padHold (1), 0.3f);
    REQUIRE (levelAt150ms() > 0.1f);
    proc.releaseResources();
}

// ---------------- REAPER: undo points and script requests ----------------

namespace fake_reaper_sync
{
    std::vector<juce::String> undoPoints;
    std::map<juce::String, juce::String> extState;
    int trackObject = 0, guidObject = 0;

    void undo (void*, const char* desc, int which, int) { REQUIRE (which == 2); undoPoints.push_back (desc); }
    const char* get (const char* section, const char* key)
    {
        static std::string held;
        const auto it = extState.find (juce::String (section) + "/" + key);
        held = it == extState.end() ? std::string() : it->second.toStdString();
        return held.c_str();
    }
    void del (const char* section, const char* key, bool) { extState.erase (juce::String (section) + "/" + key); }
    void* guid (void* track) { return track == &trackObject ? &guidObject : nullptr; }
    void guidText (const void*, char* dest) { std::strcpy (dest, "{1234-ABCD}"); }

    void install (ASamplerProcessor& proc)
    {
        undoPoints.clear();
        extState.clear();
        auto& api = proc.getReaperApi();
        api.undoOnStateChangeEx2 = undo;
        api.getExtState    = get;
        api.deleteExtState = del;
        api.getTrackGuid   = guid;
        api.guidToString   = guidText;
        api.getParent      = [] (int which) -> void* { return which == 1 ? &trackObject : nullptr; };
    }
}

TEST_CASE ("REAPER gets an undo point when samples or names change, not on restore", "[plugin][reaper][undo]")
{
    ASamplerProcessor proc;
    fake_reaper_sync::install (proc);
    auto& points = fake_reaper_sync::undoPoints;
    const auto dir = freshDir ("reaperundo");

    proc.syncWithReaper();
    REQUIRE (points.empty());   // nothing changed yet

    REQUIRE (proc.loadSampleForPadLayer (0, 0, writeWav (dir.getChildFile ("k.wav"), sine (100.0, 4410)))
             == ASamplerProcessor::LoadResult::Success);
    proc.syncWithReaper();
    REQUIRE (points.size() == 1);
    REQUIRE (points[0].startsWith ("ASampler: "));

    proc.syncWithReaper();
    REQUIRE (points.size() == 1);   // once per change

    // Many swaps in one action are one undo point.
    proc.clearAllPads();
    proc.syncWithReaper();
    REQUIRE (points.size() == 2);
    REQUIRE (points[1].contains ("Cleared all"));

    proc.setPadName (3, "Snare");
    proc.syncWithReaper();
    REQUIRE (points.size() == 3);

    // A restore (REAPER's own undo, or opening a project) is not a change.
    juce::MemoryBlock saved;
    proc.getStateInformation (saved);
    proc.setStateInformation (saved.getData(), (int) saved.getSize());
    proc.syncWithReaper();
    REQUIRE (points.size() == 3);
}

TEST_CASE ("A ReaScript can load a file onto a pad through ExtState", "[plugin][reaper][script]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    fake_reaper_sync::install (proc);
    const auto dir = freshDir ("reaperscript");
    const auto file = writeWav (dir.getChildFile ("snare.wav"), sine (200.0, 4410));

    fake_reaper_sync::extState["ASampler/load {1234-ABCD}"] = "5|" + file.getFullPathName();
    proc.syncWithReaper();
    REQUIRE (fake_reaper_sync::extState.empty());   // taken, so it runs once

    // The load itself is asynchronous, like any load from the window, and
    // announces its start.
    REQUIRE (proc.getLastStatusMessage().startsWith ("Loading snare.wav"));
    REQUIRE (proc.getLastStatusMessage().contains ("Pad 5"));

    // Another track's request is not ours.
    fake_reaper_sync::extState["ASampler/load {OTHER}"] = "1|" + file.getFullPathName();
    proc.syncWithReaper();
    REQUIRE (fake_reaper_sync::extState.size() == 1);

    // Bad requests are refused out loud.
    REQUIRE_FALSE (proc.loadFromScriptRequest ("40|" + file.getFullPathName()));
    REQUIRE (proc.getLastStatusMessage().contains ("pads 1 to 32"));
    REQUIRE_FALSE (proc.loadFromScriptRequest ("2|" + dir.getChildFile ("nope.wav").getFullPathName()));
    REQUIRE (proc.getLastStatusMessage().contains ("not found"));
    REQUIRE_FALSE (proc.loadFromScriptRequest (file.getFullPathName()));
    REQUIRE (proc.loadFromScriptRequest ("0|\"" + file.getFullPathName() + "\""));   // 0 = selected pad, quotes ok
}
