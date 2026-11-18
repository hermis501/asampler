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

// ---- REAPER automation from the plugin window ----

namespace fake_reaper_automation
{
    struct Env { bool active = false, visible = false, arm = false; };

    ASamplerProcessor* proc = nullptr;
    int  owner = 0, other = 0, master = 0;
    bool focused = true;
    int  nameShift = 0;             // a host whose list is shifted: names don't match
    int  adjustCalls = 0;
    std::map<int, Env> envelopes;   // by parameter index, on FX 2 of the owner track

    bool touched (int mode, int* track, int* item, int* take, int* fx, int* parm)
    {
        REQUIRE (mode == 1);
        if (! focused)
            return false;
        *track = 4; *item = -1; *take = -1; *fx = 2; *parm = -1;
        return true;
    }
    void* getTrack (void*, int index) { return index == 4 ? (void*) &owner : (void*) &other; }
    void* getMaster (void*)           { return &master; }
    bool paramName (void* track, int fx, int parm, char* buf, int size)
    {
        if (track != &owner || fx != 2) return false;
        auto& params = proc->getParameters();
        const int i = parm + nameShift;
        if (! juce::isPositiveAndBelow (i, params.size())) return false;
        params[i]->getName (128).copyToUTF8 (buf, (size_t) size);
        return true;
    }
    void* fxEnvelope (void* track, int fx, int parm, bool create)
    {
        if (track != &owner || fx != 2) return nullptr;
        auto it = envelopes.find (parm);
        if (it == envelopes.end())
        {
            if (! create) return nullptr;
            it = envelopes.emplace (parm, Env { true, true, false }).first;   // as REAPER makes one
        }
        return &it->second;
    }
    bool envInfo (void* envelope, const char* attr, char* buf, bool set)
    {
        auto& e = *static_cast<Env*> (envelope);
        bool* field = std::strcmp (attr, "ACTIVE") == 0  ? &e.active
                    : std::strcmp (attr, "VISIBLE") == 0 ? &e.visible
                    : std::strcmp (attr, "ARM") == 0     ? &e.arm : nullptr;
        if (field == nullptr) return false;
        if (set) *field = buf[0] == '1';
        else     std::strcpy (buf, *field ? "1" : "0");
        return true;
    }
    void adjust (bool) { ++adjustCalls; }

    void install (ASamplerProcessor& p)
    {
        proc = &p;
        focused = true;
        nameShift = 0;
        adjustCalls = 0;
        envelopes.clear();
        auto& api = p.getReaperApi();
        api.getTouchedOrFocusedFX    = touched;
        api.getTrack                 = getTrack;
        api.getMasterTrack           = getMaster;
        api.trackFXGetParamName      = paramName;
        api.getFXEnvelope            = fxEnvelope;
        api.getSetEnvelopeInfoString = envInfo;
        api.trackListAdjustWindows   = adjust;
        api.getParent = [] (int which) -> void* { return which == 1 ? &owner : nullptr; };
    }
}

TEST_CASE ("REAPER automation from the window: on, shown and armed, then off with points kept",
           "[plugin][reaper][automation]")
{
    ASamplerProcessor proc;
    auto* cutoff = proc.getState().getParameter (param_ids::padFilterCutoff (1));
    REQUIRE (cutoff != nullptr);
    const int index = proc.getParameters().indexOf (cutoff);

    // Outside REAPER: said so.
    REQUIRE_FALSE (proc.toggleReaperAutomation (cutoff));
    REQUIRE (proc.getLastStatusMessage().contains ("only in REAPER"));

    using namespace fake_reaper_automation;
    install (proc);

    REQUIRE (proc.toggleReaperAutomation (cutoff));
    REQUIRE (envelopes.count (index) == 1);
    REQUIRE (envelopes[index].active);
    REQUIRE (envelopes[index].visible);
    REQUIRE (envelopes[index].arm);
    REQUIRE (adjustCalls == 1);
    REQUIRE (proc.getLastStatusMessage() == "Automation on for " + cutoff->getName (128)
                                              + ": the envelope is shown and armed");

    REQUIRE (proc.toggleReaperAutomation (cutoff));
    REQUIRE (envelopes.count (index) == 1);   // kept, with its points
    REQUIRE_FALSE (envelopes[index].active);
    REQUIRE_FALSE (envelopes[index].visible);
    REQUIRE_FALSE (envelopes[index].arm);
    REQUIRE (proc.getLastStatusMessage().contains ("its points are kept"));

    // An existing but inactive envelope comes back on.
    REQUIRE (proc.toggleReaperAutomation (cutoff));
    REQUIRE (envelopes[index].active);

    // Another FX focused: refused, nothing touched.
    focused = false;
    REQUIRE_FALSE (proc.toggleReaperAutomation (cutoff));
    REQUIRE (proc.getLastStatusMessage().contains ("focused FX"));
    focused = true;

    // A parameter list that doesn't match by name is never written to.
    nameShift = 1;
    auto* attack = proc.getState().getParameter (param_ids::padAttack (1));
    REQUIRE_FALSE (proc.toggleReaperAutomation (attack));
    REQUIRE (envelopes.count (proc.getParameters().indexOf (attack)) == 0);
}

TEST_CASE ("Turning off all of ASampler's REAPER automation keeps the envelopes", "[plugin][reaper][automation]")
{
    ASamplerProcessor proc;
    using namespace fake_reaper_automation;
    install (proc);

    envelopes[3]   = { true,  true,  true };
    envelopes[10]  = { true,  false, false };
    envelopes[200] = { false, true,  false };
    REQUIRE (proc.disableAllReaperAutomation() == 2);
    REQUIRE (envelopes.size() == 3);
    for (const auto& e : envelopes)
    {
        REQUIRE_FALSE (e.second.active);
        REQUIRE_FALSE (e.second.arm);
    }
    REQUIRE (proc.getLastStatusMessage() == "Turned off 2 automation envelopes; their points are kept");

    REQUIRE (proc.disableAllReaperAutomation() == 0);
    REQUIRE (proc.getLastStatusMessage() == "No ASampler automation was on");

    focused = false;
    REQUIRE (proc.disableAllReaperAutomation() == -1);
}

// ---- Grabbing several REAPER items ----

namespace fake_reaper_items
{
    struct Item { juce::String guid; double position; bool selected; juce::File glued; };

    // Held by pointer and rebuilt on every undo, as REAPER may do: a pointer
    // kept across an undo would dangle.
    std::vector<std::unique_ptr<Item>> items;
    juce::File dir;
    int changeCount = 0, glues = 0, undos = 0;
    bool oneSelectedAtEachGlue = true;

    std::vector<Item*> selectedItems()
    {
        std::vector<Item*> s;
        for (auto& i : items)
            if (i->selected)
                s.push_back (i.get());
        return s;
    }
    int   countSelected (void*) { return (int) selectedItems().size(); }
    void* selectedAt (void*, int i)
    {
        auto s = selectedItems();
        return juce::isPositiveAndBelow (i, (int) s.size()) ? s[(size_t) i] : nullptr;
    }
    int   countAll (void*) { return (int) items.size(); }
    void* itemAt (void*, int i) { return juce::isPositiveAndBelow (i, (int) items.size()) ? items[(size_t) i].get() : nullptr; }
    bool  itemString (void* item, const char* parm, char* buf, bool set)
    {
        if (set || std::strcmp (parm, "GUID") != 0) return false;
        static_cast<Item*> (item)->guid.copyToUTF8 (buf, 64);
        return true;
    }
    double itemValue (void* item, const char* parm)
    {
        return std::strcmp (parm, "D_POSITION") == 0 ? static_cast<Item*> (item)->position : 0.0;
    }
    void setSelected (void* item, bool on) { static_cast<Item*> (item)->selected = on; }
    void selectAll (void*, bool on) { for (auto& i : items) i->selected = on; }
    void command (int cmd, int)
    {
        REQUIRE (cmd == 40257);
        auto s = selectedItems();
        if (s.size() != 1)
            oneSelectedAtEachGlue = false;
        for (auto* i : s)
            i->glued = writeWav (dir.getChildFile ("glued " + i->guid + ".wav"), sine (100.0, 2205));
        ++glues;
        ++changeCount;
    }
    void* take (void* item) { return item; }
    void* source (void* t) { return t; }
    void  fileName (void* src, char* buf, int size)
    {
        static_cast<Item*> (src)->glued.getFullPathName().copyToUTF8 (buf, (size_t) size);
    }
    int undo (void*)
    {
        ++undos;
        --changeCount;
        std::vector<std::unique_ptr<Item>> rebuilt;
        for (auto& i : items)
            rebuilt.push_back (std::make_unique<Item> (*i));
        items = std::move (rebuilt);
        return 1;
    }
    int stateCount (void*) { return changeCount; }

    void install (ASamplerProcessor& proc, const juce::File& folder,
                  std::initializer_list<std::pair<const char*, double>> list)
    {
        items.clear();
        for (const auto& entry : list)
            items.push_back (std::make_unique<Item> (Item { entry.first, entry.second, true, {} }));
        dir = folder;
        changeCount = glues = undos = 0;
        oneSelectedAtEachGlue = true;

        auto& api = proc.getReaperApi();
        api.countSelectedMediaItems    = countSelected;
        api.getSelectedMediaItem       = selectedAt;
        api.mainOnCommand              = command;
        api.getActiveTake              = take;
        api.getMediaItemTakeSource     = source;
        api.getMediaSourceFileName     = fileName;
        api.undoDoUndo2                = undo;
        api.getProjectStateChangeCount = stateCount;
        api.countMediaItems            = countAll;
        api.getMediaItem               = itemAt;
        api.getSetMediaItemInfoString  = itemString;
        api.getMediaItemInfoValue      = itemValue;
        api.setMediaItemSelected       = setSelected;
        api.selectAllMediaItems        = selectAll;
    }
}

TEST_CASE ("Grab puts several REAPER items on consecutive pads, each rendered alone",
           "[plugin][reaper][grab]")
{
    ASamplerProcessor proc;
    const auto dir = freshDir ("grabmany");
    using namespace fake_reaper_items;
    // Selected out of timeline order: they land in timeline order.
    install (proc, dir, { { "C", 2.0 }, { "A", 0.5 }, { "B", 1.0 } });

    // Pad 4 holds a sample already: the result says it was replaced.
    REQUIRE (proc.loadSampleForPadLayer (3, 0, writeWav (dir.getChildFile ("old.wav"), sine (300.0, 2205)))
             == ASamplerProcessor::LoadResult::Success);

    REQUIRE (proc.grabReaperItem (2, dir.getChildFile ("out")));
    REQUIRE (glues == 3);
    REQUIRE (undos == 3);
    REQUIRE (changeCount == 0);
    REQUIRE (oneSelectedAtEachGlue);          // never glued together
    REQUIRE (proc.getPadLayerSampleDisplayName (2, 0) == "Pad 3 glued A.wav");
    REQUIRE (proc.getPadLayerSampleDisplayName (3, 0) == "Pad 4 glued B.wav");
    REQUIRE (proc.getPadLayerSampleDisplayName (4, 0) == "Pad 5 glued C.wav");
    REQUIRE (proc.getLastStatusMessage() == "Grabbed 3 REAPER items onto Pads 3, 4, 5, replacing what was on Pad 4");

    // The user's selection is put back.
    REQUIRE (countSelected (nullptr) == 3);
}

TEST_CASE ("Grab stops at the last pad and says how many were left out", "[plugin][reaper][grab]")
{
    ASamplerProcessor proc;
    const auto dir = freshDir ("grabend");
    using namespace fake_reaper_items;
    install (proc, dir, { { "A", 0.0 }, { "B", 1.0 }, { "C", 2.0 } });

    REQUIRE (proc.grabReaperItem (30, dir.getChildFile ("out")));
    REQUIRE (glues == 2);
    REQUIRE (proc.getPadLayerSampleDisplayName (31, 0) == "Pad 32 glued B.wav");
    REQUIRE (proc.getLastStatusMessage()
             == "Grabbed 2 REAPER items onto Pads 31, 32. 1 item was left out: there are only 32 pads");
    REQUIRE (countSelected (nullptr) == 3);
}
