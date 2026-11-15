// Editor behaviour that matters without sight: what the controls say, and
// what the keyboard does.

#include <catch2/catch_test_macros.hpp>

#include "PluginEditor.h"

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
