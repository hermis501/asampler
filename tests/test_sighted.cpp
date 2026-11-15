// The window for sighted users: waveform, pad colours and flashes, the
// sample browser, the pad menu, window size, starter sounds and saving a pad
// as audio. None of it may take anything away from the keyboard path.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "PluginEditor.h"

using namespace asampler;

namespace
{
    juce::File freshDir (const juce::String& name)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("asampler_sighted_" + name);
        dir.deleteRecursively();
        dir.createDirectory();
        return dir;
    }

    juce::File writeTone (const juce::File& file, int numSamples = 22050)
    {
        juce::AudioBuffer<float> b (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            b.setSample (0, i, 0.5f * std::sin (0.05f * (float) i));
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (new juce::FileOutputStream (file), 44100.0, 1, 24, {}, 0));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (b, 0, numSamples);
        return file;
    }

    float plain (ASamplerProcessor& proc, const juce::String& id)
    {
        auto* p = proc.getState().getParameter (id);
        REQUIRE (p != nullptr);
        return p->convertFrom0to1 (p->getValue());
    }

    int countItems (const juce::PopupMenu& menu, juce::StringArray& names)
    {
        int n = 0;
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
        {
            const auto& item = it.getItem();
            if (item.itemID != 0)
            {
                names.add (item.text);
                ++n;
            }
        }
        return n;
    }
}

TEST_CASE ("The window scales as a whole and keeps the layout", "[editor][sighted][scale]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);

    editor.setUiScale (1.0f, false);
    REQUIRE (editor.getWidth() == ASamplerEditor::kDesignWidth);
    const auto padBounds = editor.getPadButton (0)->getBounds();

    editor.setUiScale (1.5f, false);
    REQUIRE (editor.getWidth() == juce::roundToInt (ASamplerEditor::kDesignWidth * 1.5));
    REQUIRE (std::abs (editor.getUiScale() - 1.5f) < 0.01f);
    // Laid out at the design size, drawn bigger.
    REQUIRE (editor.getPadButton (0)->getBounds() == padBounds);
    const auto onScreen = editor.getLocalArea (editor.getPadButton (0), editor.getPadButton (0)->getLocalBounds());
    REQUIRE (std::abs (onScreen.getWidth() - padBounds.getWidth() * 1.5) <= 2.0);

    editor.setUiScale (9.0f, false);   // clamped
    REQUIRE (editor.getUiScale() <= 2.5f);
}

TEST_CASE ("Fewer buttons, and every pad action is in the pad menu", "[editor][sighted][padmenu]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    editor.refreshLabelsNow();

    // Less common actions are hidden...
    auto* rename = editor.findButtonByTitlePrefix ("Rename Pad 1");
    REQUIRE (rename != nullptr);
    REQUIRE_FALSE (rename->isVisible());
    // ...and the common ones, and the menu button, are not.
    REQUIRE (editor.findButtonByTitlePrefix ("Load sample into Pad 1")->isVisible());
    auto* more = editor.findButtonByTitlePrefix ("More actions for Pad 1");
    REQUIRE (more != nullptr);
    REQUIRE (more->isVisible());

    juce::StringArray names;
    const int n = countItems (editor.buildPadMenu (0), names);
    REQUIRE (n >= 25);
    for (const auto& label : { "Rename Pad 1 (F2)", "Trim silence on Pad 1", "Snap Pad 1 to transients",
                               "Normalise Pad 1, layer 1", "Try next sample (Shift+N)", "Describe Pad 1 (I)",
                               "Save Pad 1 as audio file..." })
        REQUIRE (names.contains (label));

    // Choosing an item runs the same action as the button.
    juce::PopupMenu menu = editor.buildPadMenu (0);
    int describeId = 0;
    for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
        if (it.getItem().text == "Describe Pad 1 (I)")
            describeId = it.getItem().itemID;
    REQUIRE (describeId > 0);
    editor.runPadMenuItem (describeId);
    REQUIRE_THAT (proc.getLastStatusMessage().toStdString(), Catch::Matchers::StartsWith ("Pad 1"));

    // Shift+F10 is the keyboard way in (the menu itself opens asynchronously).
    REQUIRE (editor.keyPressed (juce::KeyPress (juce::KeyPress::F10Key, juce::ModifierKeys::shiftModifier, 0)));
    juce::PopupMenu::dismissAllActiveMenus();
}

TEST_CASE ("Pads light up when they play", "[editor][sighted][flash]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    ASamplerEditor editor (proc);

    editor.frame();
    REQUIRE (editor.getPadFlash (0) == 0.0f);

    proc.auditionPad (0, 1.0f, false);
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    proc.processBlock (buf, midi);
    REQUIRE (proc.getPadHitCount (0) >= 1);
    REQUIRE (proc.getPadPlayFraction (0) >= 0.0f);

    editor.frame();
    const float lit = editor.getPadFlash (0);
    REQUIRE (lit > 0.5f);
    REQUIRE (editor.getPadFlash (1) == 0.0f);

    for (int i = 0; i < 60; ++i)
        editor.frame();
    REQUIRE (editor.getPadFlash (0) < lit);
    proc.releaseResources();
}

TEST_CASE ("Pads report what they hold", "[plugin][sighted]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getPadContent (0) == ASamplerProcessor::PadContent::BuiltIn);

    const auto dir = freshDir ("content");
    REQUIRE (proc.loadSampleForPadLayer (0, 0, writeTone (dir.getChildFile ("a.wav")))
             == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.getPadContent (0) == ASamplerProcessor::PadContent::Loaded);
}

TEST_CASE ("The waveform shows the pad's sample and moves Start and End by drag", "[editor][sighted][waveform]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto& wave = editor.getWaveform();

    const auto dir = freshDir ("wave");
    REQUIRE (proc.loadSampleForPadLayer (0, 0, writeTone (dir.getChildFile ("a.wav"), 25600))
             == ASamplerProcessor::LoadResult::Success);
    wave.tick();
    REQUIRE (wave.getPeakBlockCount() == 100);   // 25600 / 256

    // Start sits at the left edge, End at the right.
    REQUIRE (wave.markerAt (wave.xForPosition (0.0)) == 0);
    REQUIRE (wave.markerAt (wave.xForPosition (1.0)) == 1);

    // Dragging End to the middle writes the parameter as one undoable step.
    const float endX = wave.xForPosition (1.0);
    const float midX = wave.xForPosition (0.5);
    const auto src = juce::Desktop::getInstance().getMainMouseSource();
    auto event = [&] (float x)
    {
        return juce::MouseEvent (src, { x, 50.0f }, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &wave, &wave,
                                 juce::Time::getCurrentTime(), { endX, 50.0f }, juce::Time::getCurrentTime(),
                                 1, false);
    };
    proc.getUndoManager().beginNewTransaction();
    wave.mouseDown (event (endX));
    wave.mouseDrag (event (midX));
    wave.mouseUp (event (midX));
    REQUIRE (std::abs (plain (proc, param_ids::padEnd (1)) - 0.5f) < 0.02f);

    // End can't be dragged before Start.
    const float startX = wave.xForPosition (0.0);
    wave.mouseDown (event (wave.xForPosition (0.5)));
    wave.mouseDrag (event (startX - 20.0f));
    wave.mouseUp (event (startX - 20.0f));
    REQUIRE (plain (proc, param_ids::padEnd (1)) > plain (proc, param_ids::padStart (1)));

    // Zoom keeps the point under the pointer.
    wave.zoom (0.5, 0.25);
    REQUIRE (wave.getViewEnd() - wave.getViewStart() < 0.51);
    REQUIRE (wave.getViewStart() <= 0.25);
    REQUIRE (wave.getViewEnd() >= 0.25);
}

TEST_CASE ("Envelope, filter and LFO sections show a graph; others don't", "[editor][sighted][settings]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto& panel = editor.getSettingsPanel();

    for (const auto& [section, kind] : { std::pair { "Envelope", SectionGraph::Kind::Envelope },
                                         std::pair { "Filter", SectionGraph::Kind::Filter },
                                         std::pair { "Modulation", SectionGraph::Kind::Modulation },
                                         std::pair { "Main", SectionGraph::Kind::None } })
    {
        panel.showSection (panel.getSections().indexOf (section));
        REQUIRE (panel.getGraph().getKind() == kind);
        REQUIRE (panel.getGraph().isVisible() == (kind != SectionGraph::Kind::None));
        // A picture only: nothing for the screen reader or the Tab key.
        REQUIRE_FALSE (panel.getGraph().isAccessible());
        REQUIRE_FALSE (panel.getGraph().getWantsKeyboardFocus());
    }
}

TEST_CASE ("The sample browser lists folders then audio, and loads on Enter", "[editor][sighted][browser]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto& browser = editor.getBrowser();

    const auto dir = freshDir ("browser");
    dir.getChildFile ("Kicks").createDirectory();
    writeTone (dir.getChildFile ("snare 10.wav"));
    writeTone (dir.getChildFile ("snare 9.wav"));
    dir.getChildFile ("notes.txt").replaceWithText ("not audio");

    browser.setFolder (dir);
    REQUIRE (browser.getNumEntries() == 3);
    REQUIRE (browser.getRowName (0) == "Folder Kicks");
    REQUIRE (browser.getRowName (1) == "snare 9.wav, 1 of 2");   // natural order
    REQUIRE (browser.getRowName (2) == "snare 10.wav, 2 of 2");
    REQUIRE (browser.getList().getTitle().contains ("1 folder, 2 files"));

    juce::File loaded;
    browser.onLoad = [&loaded] (const juce::File& f) { loaded = f; };
    browser.activateRow (2);
    REQUIRE (loaded == dir.getChildFile ("snare 10.wav"));

    // Enter on a folder opens it; Backspace goes up and lands on it again.
    browser.activateRow (0);
    REQUIRE (browser.getFolder() == dir.getChildFile ("Kicks"));
    REQUIRE (browser.getList().keyPressed (juce::KeyPress (juce::KeyPress::backspaceKey)));
    REQUIRE (browser.getFolder() == dir);
    REQUIRE (browser.getList().getSelectedRow() == 0);

    // A letter jumps to the next name starting with it, and is not a pad key.
    REQUIRE (browser.getList().keyPressed (juce::KeyPress ('s', {}, 's')));
    REQUIRE (browser.getList().getSelectedRow() == 1);

    // Ctrl+B swaps it in for the settings, and back.
    REQUIRE_FALSE (editor.isBrowserShown());
    REQUIRE (editor.keyPressed (juce::KeyPress ('b', juce::ModifierKeys::commandModifier, 0)));
    REQUIRE (editor.isBrowserShown());
    REQUIRE (browser.isVisible());
    REQUIRE_FALSE (editor.getSettingsPanel().isVisible());
    REQUIRE (editor.keyPressed (juce::KeyPress ('b', juce::ModifierKeys::commandModifier, 0)));
    REQUIRE_FALSE (editor.isBrowserShown());
    REQUIRE (editor.getSettingsPanel().isVisible());
}

TEST_CASE ("A pad can be saved as an audio file and dragged out as one", "[editor][sighted][saveaudio]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);

    const auto dir = freshDir ("saveaudio");
    const auto file = dir.getChildFile ("pad1.wav");
    REQUIRE (proc.savePadAudio (0, file));
    REQUIRE (file.existsAsFile());
    REQUIRE (proc.getLastStatusMessage().startsWith ("Saved Pad 1 as pad1.wav"));

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->lengthInSamples > 1000);

    // Nothing was loaded anywhere, unlike a bounce.
    for (int i = 1; i < kNumPads; ++i)
        REQUIRE (proc.getPadContent (i) == ASamplerProcessor::PadContent::BuiltIn);

    juce::StringArray files;
    bool canMove = true;
    juce::DragAndDropTarget::SourceDetails details ("pad:0", nullptr, {});
    REQUIRE (editor.shouldDropFilesWhenDraggedExternally (details, files, canMove));
    REQUIRE (files.size() == 1);
    REQUIRE (juce::File (files[0]).existsAsFile());
    REQUIRE_FALSE (canMove);
}

TEST_CASE ("Starter Drums puts synthesised sounds on pads 1 to 8, undoably", "[plugin][sighted][kit][programs]")
{
    ASamplerProcessor proc;
    const auto names = ASamplerProcessor::getFactoryKitNames();
    const int index = names.indexOf ("Starter Drums");
    REQUIRE (index == 4);   // appended: older program numbers keep their kits
    REQUIRE (proc.getProgramName (index + 1) == "Starter Drums");

    proc.getUndoManager().beginNewTransaction();
    REQUIRE (proc.loadFactoryKit (index));
    for (int i = 0; i < 8; ++i)
    {
        REQUIRE (proc.getPadContent (i) == ASamplerProcessor::PadContent::Loaded);
        REQUIRE (proc.getPadSampleFile (i).existsAsFile());
        REQUIRE (proc.getPadSampleFile (i).isAChildOf (ASamplerProcessor::getStarterSoundsFolder()));
    }
    REQUIRE (proc.getPadSampleFile (0).getFileName() == "01 Kick.wav");
    REQUIRE (proc.getPadContent (8) == ASamplerProcessor::PadContent::BuiltIn);
    REQUIRE (proc.getLastStatusMessage().contains ("pads 1 to 8"));

    // Every sound is audible and peaks at -1 dBFS.
    for (int i = 0; i < 8; ++i)
    {
        const auto s = proc.getPadDisplaySample (i);
        REQUIRE (s != nullptr);
        const float peak = s->getBuffer().getMagnitude (0, s->getNumSamples());
        REQUIRE (std::abs (juce::Decibels::gainToDecibels (peak) + 1.0f) < 0.1f);
    }

    REQUIRE (proc.undoLastAction());
    REQUIRE (proc.getPadContent (0) == ASamplerProcessor::PadContent::BuiltIn);
}

TEST_CASE ("Undo brings samples back after Reset and Clear all pads", "[plugin][undo][sighted]")
{
    ASamplerProcessor proc;
    const auto dir = freshDir ("undosamples");
    const auto a = writeTone (dir.getChildFile ("a.wav"));
    const auto b = writeTone (dir.getChildFile ("b.wav"));
    REQUIRE (proc.loadSampleForPadLayer (0, 0, a) == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (5, 0, b) == ASamplerProcessor::LoadResult::Success);

    proc.resetPadToDefault (0);
    REQUIRE (proc.getPadContent (0) == ASamplerProcessor::PadContent::BuiltIn);
    REQUIRE (proc.undoLastAction());
    REQUIRE (proc.getPadSampleFile (0) == a);
    REQUIRE (proc.redoLastAction());
    REQUIRE (proc.getPadContent (0) == ASamplerProcessor::PadContent::BuiltIn);
    REQUIRE (proc.undoLastAction());

    proc.clearAllPads();
    REQUIRE (proc.getPadContent (0) == ASamplerProcessor::PadContent::BuiltIn);
    REQUIRE (proc.getPadContent (5) == ASamplerProcessor::PadContent::BuiltIn);
    REQUIRE (proc.undoLastAction());   // one step for all 32 pads
    REQUIRE (proc.getPadSampleFile (0) == a);
    REQUIRE (proc.getPadSampleFile (5) == b);
    REQUIRE (proc.getPadDisplaySample (5)->getNumSamples() == 22050);
}

TEST_CASE ("Tab goes from the pads to the settings, the browser button, then the actions",
           "[editor][sighted][accessibility]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    editor.refreshLabelsNow();

    auto traverser = editor.createKeyboardFocusTraverser();
    REQUIRE (traverser != nullptr);

    auto* pad = editor.getPadButton (0);
    auto* next = traverser->getNextComponent (pad);
    REQUIRE (next == &editor.getSettingsPanel().getSectionBox());

    // Past the settings: the browser button, never the hidden browser or the
    // waveform.
    juce::Component* c = next;
    for (int guard = 0; guard < 200 && c != nullptr; ++guard)
    {
        if (c->getTitle().startsWith ("Show sample browser"))
            break;
        REQUIRE (c != &editor.getWaveform());
        REQUIRE_FALSE (editor.getBrowser().isParentOf (c));
        c = traverser->getNextComponent (c);
    }
    REQUIRE (c != nullptr);
    REQUIRE (c->getTitle().startsWith ("Show sample browser"));

    // Then the pad actions, starting with Load; hidden ones are skipped.
    c = traverser->getNextComponent (c);
    if (c != nullptr && dynamic_cast<juce::Label*> (c) != nullptr)
        c = traverser->getNextComponent (c);   // the "Selected" label
    REQUIRE (c != nullptr);
    REQUIRE (c->getTitle().startsWith ("Load sample into Pad 1"));
    for (int guard = 0; guard < 40 && c != nullptr && ! c->getTitle().startsWith ("More actions"); ++guard)
    {
        REQUIRE (c->isVisible());
        c = traverser->getNextComponent (c);
    }
    REQUIRE (c != nullptr);

    // With the browser shown, its list follows the pads instead.
    editor.showBrowser (true);
    REQUIRE (traverser->getNextComponent (pad) == &editor.getBrowser().getList());
}

TEST_CASE ("Tune is in the pad menu and on U", "[editor][tune]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    ASamplerEditor editor (proc);
    editor.refreshLabelsNow();

    juce::StringArray names;
    countItems (editor.buildPadMenu (0), names);
    REQUIRE (names.contains ("Tune Pad 1 (U)"));
    // Existing items keep their place.
    REQUIRE (names.contains ("Describe Pad 1 (I)"));

    // U on the built-in tone: refused, and said so.
    REQUIRE (editor.keyPressed (juce::KeyPress ('u', 0, 'u')));
    REQUIRE (proc.getLastStatusMessage() == "Pad 1 has no sample to tune");

    // F3 isn't an ASampler key: it is left to the host.
    REQUIRE_FALSE (editor.keyPressed (juce::KeyPress (juce::KeyPress::F3Key)));
    proc.releaseResources();
}
