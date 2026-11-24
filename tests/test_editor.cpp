// Editor behaviour that matters without sight: what the controls say, and
// what the keyboard does.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginEditor.h"
#include "ParameterPanel.h"

using namespace asampler;

namespace
{
    void setParam (ASamplerProcessor& proc, const juce::String& id, float value)
    {
        auto* p = proc.getState().getParameter (id);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    }
}

TEST_CASE ("Pad buttons speak the pad's real note, not the factory layout",
           "[editor][accessibility][labels]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);

    REQUIRE (editor.getPadButton (0)->getTitle().startsWith ("Pad 1, default, note C1"));

    setParam (proc, param_ids::padNote (1), 40.0f);
    editor.refreshLabelsNow();
    REQUIRE (editor.getPadButton (0)->getTitle().contains ("note E1"));
}

TEST_CASE ("Keyboard-mode pads speak their range; muted pads say so",
           "[editor][accessibility][labels]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);

    setParam (proc, param_ids::padMode (3), static_cast<float> (PadMode::Keyboard));
    setParam (proc, param_ids::padMute (3), 1.0f);
    editor.refreshLabelsNow();

    const auto title = editor.getPadButton (2)->getTitle();
    CAPTURE (title);
    REQUIRE (title.contains ("keys "));
    REQUIRE (title.contains ("muted"));
}

TEST_CASE ("Arrow keys walk the pad grid without filling the undo history",
           "[editor][keyboard][undo]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    proc.getUndoManager().clearUndoHistory();

    auto* pad1 = editor.getPadButton (0);
    REQUIRE (static_cast<juce::Component*> (pad1)->keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    REQUIRE (proc.getCurrentFocusedPad() == 1);

    // Up is higher pad numbers (Pad 1 sits bottom left).
    REQUIRE (static_cast<juce::Component*> (editor.getPadButton (1))->keyPressed (juce::KeyPress (juce::KeyPress::upKey)));
    REQUIRE (proc.getCurrentFocusedPad() == 5);

    // At an edge the key is consumed but nothing moves.
    REQUIRE (static_cast<juce::Component*> (editor.getPadButton (0))->keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));

    // Make APVTS flush parameter values into its tree now (copyState does),
    // which is where an undo step would be recorded.
    proc.getState().copyState();
    REQUIRE_FALSE (proc.getUndoManager().canUndo());

    // Only the selected pad stays in the Tab order.
    int focusable = 0;
    for (int i = 0; i < kNumPads; ++i)
        if (editor.getPadButton (i)->getWantsKeyboardFocus())
            ++focusable;
    REQUIRE (focusable == 1);
    REQUIRE (editor.getPadButton (5)->getWantsKeyboardFocus());
}

TEST_CASE ("Action buttons name the selected pad", "[editor][accessibility][labels]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);

    proc.setFocusedPad (8);
    editor.refreshLabelsNow();
    REQUIRE (editor.findButtonByTitlePrefix ("Load sample into Pad 9") != nullptr);
    REQUIRE (editor.findButtonByTitlePrefix ("Describe Pad 9") != nullptr);
    REQUIRE (editor.findButtonByTitlePrefix ("Bounce Pad 9") != nullptr);
}

TEST_CASE ("The I key describes the selected pad", "[editor][keyboard][describe]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);

    proc.setFocusedPad (3);
    REQUIRE (editor.keyPressed (juce::KeyPress ('i', {}, 'i')));
    REQUIRE (proc.getLastStatusMessage().startsWith ("Pad 4, "));
}

TEST_CASE ("Keyboard help names the shortcuts and the version", "[editor][keyboard]")
{
    const auto help = ASamplerEditor::getHelpText();
    REQUIRE (help.contains (ASamplerProcessor::getVersionString()));
    REQUIRE (help.contains ("N and P"));
    REQUIRE (help.contains ("Z X C V"));
}

TEST_CASE ("Two banks of 16 pads share the grid", "[editor][accessibility][banks]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    REQUIRE (kNumPads == 32);

    // Bank A shows; bank B is hidden, so it is not in the accessibility tree.
    proc.setFocusedPad (4);
    editor.refreshLabelsNow();
    REQUIRE (editor.getPadButton (4)->isVisible());
    REQUIRE_FALSE (editor.getPadButton (20)->isVisible());
    REQUIRE (editor.findButtonByTitlePrefix ("Bank B, pads 17 to 32") != nullptr);

    // Page Up keeps the position: Pad 5 becomes Pad 21.
    REQUIRE (static_cast<juce::Component*> (editor.getPadButton (4))
                 ->keyPressed (juce::KeyPress (juce::KeyPress::pageUpKey)));
    REQUIRE (proc.getCurrentFocusedPad() == 20);
    REQUIRE (editor.getPadButton (20)->isVisible());
    REQUIRE_FALSE (editor.getPadButton (4)->isVisible());
    REQUIRE (editor.getLastAnnouncement() == "Bank B, pads 17 to 32");

    // Up from the top row of bank A runs straight on into bank B.
    proc.setFocusedPad (13);
    editor.refreshLabelsNow();
    REQUIRE (static_cast<juce::Component*> (editor.getPadButton (13))
                 ->keyPressed (juce::KeyPress (juce::KeyPress::upKey)));
    REQUIRE (proc.getCurrentFocusedPad() == 17);

    // The pad keys play the current bank: Z is Pad 17 in bank B.
    REQUIRE (editor.getPadButton (16)->getTitle().contains ("key Z"));
    REQUIRE (editor.keyPressed (juce::KeyPress ('z', {}, 'z')));

    // Page Down from anywhere goes back to bank A.
    REQUIRE (editor.keyPressed (juce::KeyPress (juce::KeyPress::pageDownKey)));
    REQUIRE (proc.getCurrentFocusedPad() == 1);
}

namespace
{
    juce::MidiBuffer renderMidiOut (ASamplerProcessor& proc)
    {
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        proc.processBlock (buf, midi);
        return midi;
    }
}

TEST_CASE ("Pad keys play while held and send the notes out as MIDI", "[editor][keyboard][livenotes]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    ASamplerEditor editor (proc);

    const int note = proc.getPadPlayNote (0);
    REQUIRE (editor.keyPressed (juce::KeyPress ('z', {}, 'z')));
    REQUIRE (editor.keyPressed (juce::KeyPress ('z', {}, 'z')));   // auto-repeat: ignored

    auto out = renderMidiOut (proc);
    REQUIRE (out.getNumEvents() == 1);
    REQUIRE ((*out.begin()).getMessage().isNoteOn());
    REQUIRE ((*out.begin()).getMessage().getNoteNumber() == note);

    // No key is physically down in the test, so this is the release.
    editor.keyStateChanged (false);
    out = renderMidiOut (proc);
    REQUIRE (out.getNumEvents() == 1);
    REQUIRE ((*out.begin()).getMessage().isNoteOff());
}

TEST_CASE ("Ctrl and digits jump to a pad when Ctrl is released", "[editor][keyboard][jump]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);

    const juce::ModifierKeys ctrl (juce::ModifierKeys::commandModifier);
    REQUIRE (editor.keyPressed (juce::KeyPress ('1', ctrl, 0)));
    REQUIRE (editor.keyPressed (juce::KeyPress ('2', ctrl, 0)));
    REQUIRE (proc.getCurrentFocusedPad() == 0);   // not until Ctrl goes up

    editor.modifierKeysChanged (juce::ModifierKeys());
    REQUIRE (proc.getCurrentFocusedPad() == 11);
    REQUIRE (editor.getLastAnnouncement().startsWith ("Pad 12, "));

    REQUIRE (editor.keyPressed (juce::KeyPress ('9', ctrl, 0)));
    REQUIRE (editor.keyPressed (juce::KeyPress ('9', ctrl, 0)));
    editor.modifierKeysChanged (juce::ModifierKeys());
    REQUIRE (proc.getCurrentFocusedPad() == 11);
    REQUIRE (editor.getLastAnnouncement() == "No pad 99, there are 32");
}

TEST_CASE ("Shift and a pad key speak the pad, with its name, without playing it",
           "[editor][keyboard][names]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    ASamplerEditor editor (proc);
    proc.setPadName (1, "Snare");

    REQUIRE (editor.keyPressed (juce::KeyPress ('X', juce::ModifierKeys (juce::ModifierKeys::shiftModifier), 'X')));
    REQUIRE (editor.getLastAnnouncement().startsWith ("Pad 2, Snare, default"));
    REQUIRE (renderMidiOut (proc).isEmpty());

    editor.refreshLabelsNow();
    REQUIRE (editor.getPadButton (1)->getButtonText() == "2  Snare");
}

TEST_CASE ("T edits Start by ear: arrows step, Enter keeps, Escape restores", "[editor][keyboard][scrub]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    ASamplerEditor editor (proc);

    REQUIRE (editor.keyPressed (juce::KeyPress ('t', {}, 't')));
    REQUIRE (editor.getMode() == ASamplerEditor::Mode::EditStart);
    REQUIRE (editor.getLastAnnouncement().startsWith ("Editing Start of Pad 1"));
    REQUIRE (editor.findButtonByTitlePrefix ("Keep Start of Pad 1") != nullptr);

    const auto shift = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);
    REQUIRE (editor.keyPressed (juce::KeyPress (juce::KeyPress::rightKey, shift, 0)));
    REQUIRE (proc.isPreviewPlaying());
    REQUIRE (proc.getSlicePoint (0, false) == 0.0f);   // nothing written yet

    SECTION ("Enter keeps")
    {
        REQUIRE (editor.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
        REQUIRE (editor.getMode() == ASamplerEditor::Mode::Normal);
        REQUIRE (proc.getSlicePoint (0, false) > 0.0f);
        REQUIRE (proc.getLastStatusMessage().startsWith ("Pad 1 Start"));
    }

    SECTION ("Escape restores")
    {
        REQUIRE (editor.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
        REQUIRE (editor.getMode() == ASamplerEditor::Mode::Normal);
        REQUIRE (proc.getSlicePoint (0, false) == 0.0f);
        REQUIRE (editor.getLastAnnouncement().startsWith ("Start unchanged"));
    }
}

TEST_CASE ("K arms note learn and Escape cancels it", "[editor][keyboard][notelearn]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);

    proc.setFocusedPad (2);
    REQUIRE (editor.keyPressed (juce::KeyPress ('k', {}, 'k')));
    REQUIRE (proc.isNoteLearnArmed());
    REQUIRE (proc.getLastStatusMessage().startsWith ("Play a note to set Pad 3 Note"));
    REQUIRE (editor.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
    REQUIRE_FALSE (proc.isNoteLearnArmed());
}

TEST_CASE ("Keyboard help covers the new shortcuts", "[editor][keyboard]")
{
    const auto help = ASamplerEditor::getHelpText();
    REQUIRE (help.contains ("Shift N"));
    REQUIRE (help.contains ("T edits Start"));
    REQUIRE (help.contains ("F2 renames"));
    REQUIRE (help.contains ("grabs the selected item"));
    REQUIRE (help.contains ("selected pad's settings"));
}

// ---- Settings panel ---------------------------------------------------------

namespace
{
    float plain (ASamplerProcessor& proc, const juce::String& id)
    {
        auto* p = proc.getState().getParameter (id);
        REQUIRE (p != nullptr);
        return p->convertFrom0to1 (p->getValue());
    }

    bool press (juce::Component* c, int keyCode, juce::ModifierKeys mods = {})
    {
        REQUIRE (c != nullptr);
        return c->keyPressed (juce::KeyPress (keyCode, mods, 0));
    }
}

TEST_CASE ("The settings panel shows the selected pad's settings by section",
           "[editor][settings][accessibility]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto& panel = editor.getSettingsPanel();

    // Same sections as the Focus group, Main first.
    REQUIRE (panel.getSections()[0] == "Main");
    REQUIRE (panel.getSections().contains ("Stereo"));
    REQUIRE (panel.getSections().contains ("Filter"));
    REQUIRE (panel.getSectionBox().getTitle() == "Settings section");

    // Knobs, lists and checkboxes by the parameter's kind, titled by name.
    REQUIRE (dynamic_cast<juce::Slider*>       (panel.findControl ("Volume"))  != nullptr);
    REQUIRE (dynamic_cast<juce::ToggleButton*> (panel.findControl ("Reverse")) != nullptr);
    REQUIRE (panel.findControl ("Filter Cutoff") == nullptr);   // another section

    panel.showSection (panel.getSections().indexOf ("Trigger"));
    REQUIRE (dynamic_cast<juce::ComboBox*> (panel.findControl ("Mode")) != nullptr);
    REQUIRE (panel.findControl ("Volume") == nullptr);

    // The section list drives the same thing.
    panel.getSectionBox().setSelectedItemIndex (panel.getSections().indexOf ("Filter"), juce::sendNotificationSync);
    REQUIRE (panel.getSectionIndex() == panel.getSections().indexOf ("Filter"));
    REQUIRE (panel.findControl ("Filter Cutoff") != nullptr);
}

TEST_CASE ("Settings knobs change the selected pad and follow it to another pad",
           "[editor][settings][keyboard]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto& panel = editor.getSettingsPanel();

    auto* pan = dynamic_cast<juce::Slider*> (panel.findControl ("Pan"));
    REQUIRE (pan != nullptr);

    // One arrow press is a usable step (1% of the range), not the
    // parameter's tiny interval; Shift is ten times that.
    const float before = plain (proc, param_ids::padPan (1));
    REQUIRE (press (pan, juce::KeyPress::rightKey));
    const float oneStep = plain (proc, param_ids::padPan (1)) - before;
    REQUIRE (oneStep > 0.015f);
    REQUIRE (oneStep < 0.025f);
    REQUIRE (press (pan, juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier));
    REQUIRE (plain (proc, param_ids::padPan (1)) - before > 0.2f);

    // Home and End reach the ends.
    REQUIRE (press (pan, juce::KeyPress::homeKey));
    REQUIRE (plain (proc, param_ids::padPan (1)) == -1.0f);

    // The same knob now works on Pad 3, and Pad 1 keeps its value.
    proc.setFocusedPad (2);   // as the grid, the host or Ctrl+3 would
    editor.refreshLabelsNow();
    REQUIRE (panel.getPad() == 2);
    REQUIRE (panel.findControl ("Pan") == pan);   // rebound, not rebuilt: focus survives

    REQUIRE (press (pan, juce::KeyPress::endKey));
    REQUIRE (plain (proc, param_ids::padPan (3)) == 1.0f);
    REQUIRE (plain (proc, param_ids::padPan (1)) == -1.0f);

    // Its value reads as the host list does.
    REQUIRE (pan->getTextFromValue (pan->getValue())
             == proc.getState().getParameter (param_ids::padPan (3))->getCurrentValueAsText());
}

TEST_CASE ("Settings checkboxes and lists write the parameter", "[editor][settings]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto& panel = editor.getSettingsPanel();

    auto* reverse = dynamic_cast<juce::Button*> (panel.findControl ("Reverse"));
    REQUIRE (reverse != nullptr);
    reverse->setToggleState (true, juce::sendNotificationSync);
    REQUIRE (plain (proc, param_ids::padReverse (1)) == 1.0f);

    panel.showSection (panel.getSections().indexOf ("Trigger"));
    auto* modeBox = dynamic_cast<juce::ComboBox*> (panel.findControl ("Mode"));
    REQUIRE (modeBox != nullptr);
    modeBox->setSelectedItemIndex (static_cast<int> (PadMode::Keyboard), juce::sendNotificationSync);
    REQUIRE (juce::roundToInt (plain (proc, param_ids::padMode (1))) == static_cast<int> (PadMode::Keyboard));
}

TEST_CASE ("While editing Start by ear, arrows on a settings knob move Start, not the knob",
           "[editor][settings][scrub]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto* volume = editor.getSettingsPanel().findControl ("Volume");
    const float before = plain (proc, param_ids::padVolume (1));

    REQUIRE (editor.keyPressed (juce::KeyPress ('t', {}, 't')));
    REQUIRE (editor.getMode() == ASamplerEditor::Mode::EditStart);
    REQUIRE (press (volume, juce::KeyPress::rightKey));
    REQUIRE (plain (proc, param_ids::padVolume (1)) == before);
    REQUIRE (press (volume, juce::KeyPress::escapeKey));
    REQUIRE (editor.getMode() == ASamplerEditor::Mode::Normal);
}

TEST_CASE ("The settings panel comes right after the pads in the Tab order",
           "[editor][settings][accessibility]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);

    const int panelOrder = editor.getSettingsPanel().getExplicitFocusOrder();
    REQUIRE (panelOrder > editor.getPadButton (kNumPads - 1)->getExplicitFocusOrder());
    auto* load = editor.findButtonByTitlePrefix ("Load");
    REQUIRE (load != nullptr);
    REQUIRE (panelOrder < load->getExplicitFocusOrder());
}

// Hidden: writes the editor as it looks to <temp>/asampler-editor.png, to check
// the layout without opening a host. Run with "[.snapshot]".
TEST_CASE ("Editor snapshot", "[.snapshot]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    proc.loadFactoryKit (ASamplerProcessor::getFactoryKitNames().indexOf ("Starter Drums"));
    ASamplerEditor editor (proc);
    editor.setUiScale (1.0f, false);
    editor.getSettingsPanel().showSection (editor.getSettingsPanel().getSections().indexOf ("Filter"));
    editor.setVisible (true);

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("asampler-editor.png");
    file.deleteFile();
    juce::FileOutputStream out (file);
    REQUIRE (out.openedOk());
    juce::PNGImageFormat().writeImageToStream (editor.createComponentSnapshot (editor.getLocalBounds()), out);
    out.flush();
    WARN (file.getFullPathName());
}

TEST_CASE ("The Layers section is split into one page per layer", "[editor][settings][layers]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto& panel = editor.getSettingsPanel();

    for (int L = 1; L <= kNumLayers; ++L)
        REQUIRE (panel.getSections().contains ("Layer " + juce::String (L)));
    REQUIRE (panel.getSections().contains ("Layers"));

    panel.showSection (panel.getSections().indexOf ("Layer 3"));
    REQUIRE (panel.findControl ("Layer 3 Gain") != nullptr);
    REQUIRE (panel.findControl ("Layer 2 Gain") == nullptr);

    // Every control is on exactly one page.
    int total = 0;
    for (int s = 0; s < panel.getSections().size(); ++s)
    {
        panel.showSection (s);
        for (int i = 0; i < ASamplerProcessor::getNumPadControls(); ++i)
            if (panel.findControl (ASamplerProcessor::getPadControlName (i)) != nullptr)
                ++total;
    }
    REQUIRE (total == ASamplerProcessor::getNumPadControls());
}

TEST_CASE ("Find setting matches names in any section, best first", "[editor][findsetting]")
{
    auto names = [] (const juce::String& q)
    {
        juce::StringArray out;
        for (int i : ParameterPanel::findSettings (q))
            out.add (ParameterPanel::getSettingName (i));
        return out;
    };

    // Name starts first, then word starts, then anywhere.
    const auto cutoff = names ("cutoff");
    REQUIRE (cutoff.contains ("Filter Cutoff"));
    REQUIRE (names ("filter c")[0] == "Filter Cutoff");
    REQUIRE (names ("rel")[0] == "Release");
    REQUIRE (names ("env rel").contains ("Filter Env Release"));
    REQUIRE (names ("lfo 2 rate")[0] == "LFO 2 Rate");
    REQUIRE (names ("  ").isEmpty());
    REQUIRE (names ("zzzz").isEmpty());
    // Case doesn't matter.
    REQUIRE (names ("FILTER CUTOFF") == names ("filter cutoff"));
}

namespace
{
    // Types into a text box the way keys arrive: one keyPressed per character.
    void typeInto (juce::TextEditor& box, const juce::String& text)
    {
        box.setText ({}, false);
        for (auto c : text)
            box.keyPressed (juce::KeyPress ((int) c, 0, c));
    }
}

TEST_CASE ("Ctrl+F finds a setting and goes to its section", "[editor][findsetting]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto& panel = editor.getSettingsPanel();
    juce::StringArray spoken;
    panel.onAnnounce = [&spoken] (const juce::String& s) { spoken.add (s); };

    REQUIRE (panel.getSections()[panel.getSectionIndex()] != "Filter");
    REQUIRE (editor.keyPressed (juce::KeyPress ('f', juce::ModifierKeys::commandModifier, 0)));
    REQUIRE (panel.isSearching());

    auto& box = panel.getSearchBox();
    typeInto (box, "filter cut");
    REQUIRE_FALSE (spoken.isEmpty());
    REQUIRE_THAT (spoken.getReference (spoken.size() - 1).toStdString(),
                  Catch::Matchers::StartsWith ("Filter Cutoff, Filter, 1 of"));

    // Down moves to the next match, Up comes back.
    typeInto (box, "release");
    const int before = spoken.size();
    REQUIRE (box.keyPressed (juce::KeyPress (juce::KeyPress::downKey)));
    REQUIRE (spoken.size() == before + 1);
    REQUIRE_THAT (spoken.getReference (spoken.size() - 1).toStdString(), Catch::Matchers::ContainsSubstring ("2 of"));
    REQUIRE (box.keyPressed (juce::KeyPress (juce::KeyPress::upKey)));
    REQUIRE_THAT (spoken.getReference (spoken.size() - 1).toStdString(), Catch::Matchers::StartsWith ("Release, Envelope, 1 of"));

    // Enter goes there and ends the search.
    typeInto (box, "filter cutoff");
    REQUIRE (box.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
    REQUIRE_FALSE (panel.isSearching());
    REQUIRE (panel.getSections()[panel.getSectionIndex()] == "Filter");
    REQUIRE (panel.findControl ("Filter Cutoff") != nullptr);

    // No match: said so, and Enter stays in the search.
    REQUIRE (editor.keyPressed (juce::KeyPress ('f', juce::ModifierKeys::commandModifier, 0)));
    typeInto (box, "qqq");
    REQUIRE (spoken.getReference (spoken.size() - 1) == "No setting matches qqq");
    REQUIRE (box.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
    REQUIRE (panel.isSearching());

    // Escape ends it without moving anywhere.
    REQUIRE (box.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
    REQUIRE_FALSE (panel.isSearching());
    REQUIRE (panel.getSections()[panel.getSectionIndex()] == "Filter");
}

TEST_CASE ("Ctrl+F from the sample browser brings the settings back", "[editor][findsetting]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    REQUIRE (editor.keyPressed (juce::KeyPress ('b', juce::ModifierKeys::commandModifier, 0)));
    REQUIRE_FALSE (editor.getSettingsPanel().isVisible());
    REQUIRE (editor.keyPressed (juce::KeyPress ('f', juce::ModifierKeys::commandModifier, 0)));
    REQUIRE (editor.getSettingsPanel().isVisible());
    REQUIRE (editor.getSettingsPanel().isSearching());
}

TEST_CASE ("The Global section has every plugin-wide setting, last in the list", "[editor][settings][global]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    ASamplerEditor editor (proc);
    auto& panel = editor.getSettingsPanel();

    // Appended, so the pad sections keep their places.
    REQUIRE (panel.getSections()[0] == "Main");
    REQUIRE (panel.getSections()[panel.getSections().size() - 1] == "Global");
    panel.showSection (panel.getSections().indexOf ("Global"));

    // Every host parameter is in the window: each pad control in a pad
    // section (checked elsewhere), every other parameter here, except
    // Focused Pad (the grid selects pads) and the Focus proxies.
    juce::StringArray globalNames;
    for (int i = ASamplerProcessor::getNumPadControls(); i < ParameterPanel::getNumSettings(); ++i)
        globalNames.add (ParameterPanel::getSettingName (i));

    for (auto* p : proc.getParameters())
    {
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p);
        REQUIRE (withId != nullptr);
        const auto& id = withId->paramID;
        if (id.startsWith ("pad_") || id.startsWith ("focus_") || id == param_ids::focusedPad)
            continue;
        INFO (id);
        REQUIRE (globalNames.contains (p->getName (128)));
        REQUIRE (panel.findControl (p->getName (128)) != nullptr);
    }

    // The heading says these aren't one pad's.
    REQUIRE (panel.findControl ("Master Gain") != nullptr);
    REQUIRE (panel.findControl ("Filter Cutoff") == nullptr);

    // A knob here changes the global parameter, as an undoable change.
    auto* gainKnob = dynamic_cast<juce::Slider*> (panel.findControl ("Master Gain"));
    REQUIRE (gainKnob != nullptr);
    gainKnob->setValue (-12.0, juce::sendNotificationSync);
    REQUIRE (proc.getState().getRawParameterValue (param_ids::masterGain)->load() == Catch::Approx (-12.0f));

    // Changing pad keeps the global controls bound to the same parameters.
    panel.setPad (5);
    REQUIRE (dynamic_cast<juce::Slider*> (panel.findControl ("Master Gain"))->getValue() == Catch::Approx (-12.0));
}

TEST_CASE ("Find setting reaches the global settings", "[editor][findsetting][global]")
{
    auto names = [] (const juce::String& q)
    {
        juce::StringArray out;
        for (int i : ParameterPanel::findSettings (q))
            out.add (ParameterPanel::getSettingName (i));
        return out;
    };
    REQUIRE (names ("master")[0] == "Master Gain");
    REQUIRE (names ("edit layer")[0] == "Edit Layer");
    REQUIRE (names ("chop c")[0] == "Chop Count");
}

TEST_CASE ("MIDI learn from the pad menu binds the setting last used", "[editor][midilearn]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    ASamplerEditor editor (proc);
    auto& panel = editor.getSettingsPanel();

    auto learnItem = [&]
    {
        auto menu = editor.buildPadMenu (0);
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
            if (it.getItem().text.startsWith ("MIDI learn"))
                return it.getItem();
        return juce::PopupMenu::Item();
    };

    // Nothing used yet: the item is there but says what to do first.
    REQUIRE_FALSE (learnItem().isEnabled);
    REQUIRE (learnItem().text.contains ("first move to a setting"));
    REQUIRE_FALSE (proc.armMidiLearn (nullptr));

    panel.showSection (panel.getSections().indexOf ("Filter"));
    auto* cutoff = panel.findControl ("Filter Cutoff");
    REQUIRE (cutoff != nullptr);
    // An editor with no window can't take focus; deliver the focus callback
    // a click or Tab would.
    cutoff->focusGained (juce::Component::focusChangedDirectly);

    auto item = learnItem();
    REQUIRE (item.isEnabled);
    auto* param = proc.getState().getParameter (param_ids::padFilterCutoff (1));
    REQUIRE (item.text.contains (param->getName (128)));

    editor.runPadMenuItem (item.itemID);
    REQUIRE (proc.getLastStatusMessage().startsWith ("MIDI learn: move a controller"));

    // The next controller binds to it, and then moves it.
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 20, 127), 0);
    proc.processBlock (buf, midi);
    REQUIRE (proc.getLastStatusMessage() == "Bound CC 20 to " + param->getName (128));
    REQUIRE (param->getValue() == Catch::Approx (1.0f));

    midi.clear();
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 20, 0), 0);
    proc.processBlock (buf, midi);
    REQUIRE (param->getValue() == Catch::Approx (0.0f));
}
