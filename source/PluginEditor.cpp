#include "PluginEditor.h"

namespace asampler
{
    namespace
    {
        constexpr int kRowHeight    = 28;
        constexpr int kRowSpacing   = 4;
        constexpr int kPadding      = 12;
        constexpr int kHeadingHeight = 32;
        constexpr int kStatusHeight  = 32;
        constexpr int kLoadButtonWidth     = 300;
        constexpr int kAuditionButtonWidth = 110;
        constexpr int kResetButtonWidth    = 140;
        constexpr int kChopButtonWidth     = 200;
        constexpr int kButtonGap           = 8;
        constexpr int kUtilityButtonWidth  = 132;

        juce::String midiNoteName (int note)
        {
            return juce::MidiMessage::getMidiNoteName (note, true, true, 3);
        }

        // MPC-style QWERTY pad layout: the bottom keyboard row is pads 1-4 and
        // each row above it steps up a bank of four, matching how the pads are
        // numbered on hardware.
        constexpr const char* kPadKeyRows[4] = { "zxcv", "asdf", "qwer", "1234" };
    }

    ASamplerEditor::ASamplerEditor (ASamplerProcessor& proc)
        : juce::AudioProcessorEditor (proc),
          processor (proc)
    {
        setWantsKeyboardFocus (true);

        // Heading. Marked as static text so a screen reader skim-navigates
        // landmarks rather than reading it as if it were interactive.
        heading.setText ("ASampler - Pad Sample Loader", juce::dontSendNotification);
        heading.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
        heading.setJustificationType (juce::Justification::centredLeft);
        heading.setAccessible (true);
        heading.setTitle ("ASampler Pad Sample Loader");
        heading.setDescription ("Headings for each pad follow. Tab to navigate Load and Reset buttons.");
        addAndMakeVisible (heading);

        // Vocal generator row. TextEditor exposes an editable-text UIA role, and
        // the three buttons expose Button roles — all screen-reader navigable.
        vocalLabel.setText ("Vocal generator (SAPI5 text to speech)", juce::dontSendNotification);
        vocalLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        vocalLabel.setJustificationType (juce::Justification::centredLeft);
        vocalLabel.setAccessible (true);
        vocalLabel.setTitle ("Vocal generator");
        addAndMakeVisible (vocalLabel);

        vocalEditor.setMultiLine (false);
        vocalEditor.setReturnKeyStartsNewLine (false);
        vocalEditor.setTextToShowWhenEmpty ("Type words to speak, then Generate",
                                            juce::Colours::grey);
        vocalEditor.setWantsKeyboardFocus (true);
        vocalEditor.setTitle ("Vocal text");
        vocalEditor.setDescription ("Type text, then use a Generate button to speak it onto the target pad.");
        addAndMakeVisible (vocalEditor);

        vocalSpeakButton.setWantsKeyboardFocus (true);
        vocalSpeakButton.onClick = [this] { onVocalSpeakClicked(); };
        addAndMakeVisible (vocalSpeakButton);

        vocalChopButton.setWantsKeyboardFocus (true);
        vocalChopButton.onClick = [this] { onVocalChopClicked(); };
        addAndMakeVisible (vocalChopButton);

        vocalSpreadButton.setWantsKeyboardFocus (true);
        vocalSpreadButton.onClick = [this] { onVocalSpreadClicked(); };
        addAndMakeVisible (vocalSpreadButton);

        // Keep the vocal button labels naming the current target pad.
        if (auto* targetParam = processor.getState().getParameter (param_ids::vocalTargetPad))
        {
            vocalTargetAttachment = std::make_unique<juce::ParameterAttachment> (
                *targetParam,
                [this] (float) { refreshVocalLabels(); });
            vocalTargetAttachment->sendInitialUpdate();
        }

        // Kit / utility row.
        utilityLabel.setText ("Kit and pad tools", juce::dontSendNotification);
        utilityLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        utilityLabel.setJustificationType (juce::Justification::centredLeft);
        utilityLabel.setAccessible (true);
        utilityLabel.setTitle ("Kit and pad tools");
        addAndMakeVisible (utilityLabel);

        addButton (undoButton, "Undo", [this] { onUndoClicked(); },
                   "Undo the last parameter change, including a chop across pads.");
        addButton (redoButton, "Redo", [this] { onRedoClicked(); });
        addButton (saveKitButton, "Save kit...", [this] { onSaveKitClicked(); },
                   "Save all pad settings and sample paths to a kit file.");
        addButton (loadKitButton, "Load kit...", [this] { onLoadKitClicked(); });
        addButton (factoryKitButton, "Factory kit...", [this] { onFactoryKitClicked(); },
                   "Load a built-in starting point. Changes settings only, not samples.");
        addButton (relinkButton, "Relink samples...", [this] { onRelinkClicked(); },
                   "Search a folder for samples this project could not find.");
        addButton (copyPadButton, "Copy pad", [this] { onCopyPadClicked(); });
        addButton (pastePadButton, "Paste pad", [this] { onPastePadClicked(); });
        addButton (clearAllButton, "Clear all pads", [this] { onClearAllClicked(); },
                   "Reset every pad to the default sample. Undo restores them.");
        addButton (exportMidiButton, "Export slice MIDI...", [this] { onExportMidiClicked(); },
                   "Write a MIDI file with one note per slice, to import into your project.");
        addButton (normaliseButton, "Normalise layer", [this] { onNormaliseClicked(); },
                   "Set the focused pad's layer gain so its peak sits at -1 dB.");
        addButton (snapTransientsButton, "Snap to transients",
                   [this] { onSnapTransientsClicked(); },
                   "Move the focused pad's start and end onto the nearest detected onsets.");
        addButton (renderStretchButton, "Render stretch",
                   [this] { onRenderStretchClicked(); },
                   "Apply the focused pad's time-stretch and formant settings now, "
                   "instead of waiting for the background render.");
        addButton (renderFormantsButton, "Render formants",
                   [this] { onRenderFormantsClicked(); },
                   "Build the per-note formant correction for the focused pad now, "
                   "so formants stay fixed as it is played across the keyboard.");

        for (int i = 0; i < kNumPads; ++i)
        {
            auto load = std::make_unique<juce::TextButton>();
            load->setWantsKeyboardFocus (true);
            load->onClick = [this, i] { onLoadClicked (i); };
            addAndMakeVisible (load.get());
            loadButtons[i] = std::move (load);

            auto audition = std::make_unique<juce::TextButton>();
            audition->setWantsKeyboardFocus (true);
            audition->onClick = [this, i] { onAuditionClicked (i); };
            addAndMakeVisible (audition.get());
            auditionButtons[i] = std::move (audition);

            auto reset = std::make_unique<juce::TextButton>();
            reset->setWantsKeyboardFocus (true);
            reset->setButtonText ("Pad " + juce::String (i + 1) + ": Reset to default");
            reset->setTitle (reset->getButtonText());
            reset->onClick = [this, i] { onResetClicked (i); };
            addAndMakeVisible (reset.get());
            resetButtons[i] = std::move (reset);

            auto chop = std::make_unique<juce::TextButton>();
            chop->setWantsKeyboardFocus (true);
            chop->onClick = [this, i] { onChopClicked (i); };
            addAndMakeVisible (chop.get());
            chopButtons[i] = std::move (chop);
        }

        // Keep the pad-scoped utility buttons naming the focused pad.
        if (auto* focusParam = processor.getState().getParameter (param_ids::focusedPad))
        {
            focusedPadAttachment = std::make_unique<juce::ParameterAttachment> (
                *focusParam,
                [this] (float) { refreshUtilityLabels(); });
            focusedPadAttachment->sendInitialUpdate();
        }

        // Refresh chop-button labels whenever the host's Chop Count value moves.
        // Falling back gracefully if the parameter can't be resolved keeps the
        // editor functional in tests that mock the processor.
        if (auto* chopParam = processor.getState().getParameter (param_ids::chopCount))
        {
            chopCountAttachment = std::make_unique<juce::ParameterAttachment> (
                *chopParam,
                [this] (float) { refreshPadButtonLabels(); });
            chopCountAttachment->sendInitialUpdate();
        }

        // Same for Edit Layer — moving it must refresh the per-pad Load labels
        // so the screen reader speaks the sample currently on the active layer.
        if (auto* layerParam = processor.getState().getParameter (param_ids::editLayer))
        {
            editLayerAttachment = std::make_unique<juce::ParameterAttachment> (
                *layerParam,
                [this] (float) { refreshPadButtonLabels(); });
            editLayerAttachment->sendInitialUpdate();
        }

        statusLabel.setText ("Ready. Tab to a pad to load a sample, "
                             "or press Z X C V / A S D F / Q W E R / 1 2 3 4 to audition pads 1 to 16.",
                             juce::dontSendNotification);
        statusLabel.setFont (juce::Font (juce::FontOptions (14.0f)));
        statusLabel.setJustificationType (juce::Justification::centredLeft);
        statusLabel.setAccessible (true);
        statusLabel.setTitle ("Status");
        addAndMakeVisible (statusLabel);

        refreshVocalLabels();
        refreshUtilityLabels();
        refreshPadButtonLabels();
        refreshStatusLabel();

        processor.addChangeListener (this);

        const int rowsHeight  = (kRowHeight + kRowSpacing) * kNumPads;
        // Vocal block: a label row + an editor/buttons row, each with trailing spacing.
        const int vocalHeight = (kRowHeight + kRowSpacing) * 2;
        // Utility block: a label row + two rows of buttons.
        const int utilityHeight = (kRowHeight + kRowSpacing) * 3;

        const int contentWidth = kLoadButtonWidth + kButtonGap + kAuditionButtonWidth
                               + kButtonGap + kResetButtonWidth + kButtonGap
                               + kChopButtonWidth;

        setSize (contentWidth + 2 * kPadding,
                 kPadding + kHeadingHeight + kRowSpacing + vocalHeight + utilityHeight
                     + rowsHeight + kStatusHeight + kPadding);
    }

    juce::TextButton& ASamplerEditor::addButton (std::unique_ptr<juce::TextButton>& slot,
                                                 const juce::String& text,
                                                 std::function<void()> onClick,
                                                 const juce::String& description)
    {
        slot = std::make_unique<juce::TextButton>();
        slot->setWantsKeyboardFocus (true);
        slot->setButtonText (text);
        // The accessible title takes precedence over the visible text for
        // screen readers, so the two must always be set together.
        slot->setTitle (text);
        if (description.isNotEmpty())
            slot->setDescription (description);
        slot->onClick = std::move (onClick);
        addAndMakeVisible (slot.get());
        return *slot;
    }

    ASamplerEditor::~ASamplerEditor()
    {
        processor.removeChangeListener (this);
    }

    void ASamplerEditor::paint (juce::Graphics& g)
    {
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    }

    void ASamplerEditor::resized()
    {
        auto area = getLocalBounds().reduced (kPadding);

        heading.setBounds (area.removeFromTop (kHeadingHeight));
        area.removeFromTop (kRowSpacing);

        // Vocal generator: label row, then [ text editor | 3 buttons ] row.
        vocalLabel.setBounds (area.removeFromTop (kRowHeight));
        area.removeFromTop (kRowSpacing);
        {
            auto vrow = area.removeFromTop (kRowHeight);
            const int vbw = 150;   // vocal button width
            vocalSpreadButton.setBounds (vrow.removeFromRight (vbw));
            vrow.removeFromRight (kButtonGap);
            vocalChopButton.setBounds (vrow.removeFromRight (vbw));
            vrow.removeFromRight (kButtonGap);
            vocalSpeakButton.setBounds (vrow.removeFromRight (vbw));
            vrow.removeFromRight (kButtonGap);
            vocalEditor.setBounds (vrow);   // remaining width on the left
        }
        area.removeFromTop (kRowSpacing);

        // Kit / utility: label row, then the buttons flowed across two rows.
        utilityLabel.setBounds (area.removeFromTop (kRowHeight));
        area.removeFromTop (kRowSpacing);
        {
            juce::TextButton* utils[] = {
                undoButton.get(), redoButton.get(), saveKitButton.get(),
                loadKitButton.get(), factoryKitButton.get(), relinkButton.get(),
                copyPadButton.get(), pastePadButton.get(), clearAllButton.get(),
                exportMidiButton.get(), normaliseButton.get(),
                snapTransientsButton.get(), renderStretchButton.get(),
                renderFormantsButton.get()
            };

            auto row = area.removeFromTop (kRowHeight);
            for (auto* b : utils)
            {
                if (b == nullptr) continue;

                if (row.getWidth() < kUtilityButtonWidth)
                {
                    area.removeFromTop (kRowSpacing);
                    row = area.removeFromTop (kRowHeight);
                }
                b->setBounds (row.removeFromLeft (kUtilityButtonWidth));
                row.removeFromLeft (kButtonGap);
            }
            area.removeFromTop (kRowSpacing);
        }

        for (int i = 0; i < kNumPads; ++i)
        {
            auto row = area.removeFromTop (kRowHeight);
            loadButtons[i]    ->setBounds (row.removeFromLeft (kLoadButtonWidth));
            row.removeFromLeft (kButtonGap);
            auditionButtons[i]->setBounds (row.removeFromLeft (kAuditionButtonWidth));
            row.removeFromLeft (kButtonGap);
            resetButtons[i]   ->setBounds (row.removeFromLeft (kResetButtonWidth));
            row.removeFromLeft (kButtonGap);
            chopButtons[i]    ->setBounds (row.removeFromLeft (kChopButtonWidth));
            area.removeFromTop (kRowSpacing);
        }

        statusLabel.setBounds (area.removeFromTop (kStatusHeight));
    }

    void ASamplerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        refreshUtilityLabels();
        refreshPadButtonLabels();
        refreshStatusLabel();
        announce (processor.getLastStatusMessage());
    }

    void ASamplerEditor::onLoadClicked (int padIndex)
    {
        const int  padNumber = padIndex + 1;
        const auto title     = "Choose a sample for Pad " + juce::String (padNumber);

        fileChooser = std::make_unique<juce::FileChooser> (
            title,
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
            "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

        const auto flags = juce::FileBrowserComponent::openMode
                         | juce::FileBrowserComponent::canSelectFiles;

        fileChooser->launchAsync (flags,
            [this, padIndex] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file == juce::File{})
                    return;   // user cancelled — no announcement needed

                // Async so a long file never freezes the host. The processor
                // announces both "Loading..." and the result.
                processor.loadSampleForPadLayerAsync (
                    padIndex, processor.getCurrentEditLayer(), file);
            });
    }

    void ASamplerEditor::onAuditionClicked (int padIndex)
    {
        processor.auditionPad (padIndex);
    }

    void ASamplerEditor::onResetClicked (int padIndex)
    {
        processor.resetPadToDefault (padIndex);
    }

    void ASamplerEditor::onChopClicked (int padIndex)
    {
        processor.chopAcrossPads (padIndex);
        // The processor sets a status message describing the outcome; the
        // existing ChangeListener path forwards it to the screen reader.
    }

    // ---- Kit / utility actions ----

    void ASamplerEditor::onUndoClicked() { processor.undoLastAction(); }
    void ASamplerEditor::onRedoClicked() { processor.redoLastAction(); }

    void ASamplerEditor::onSaveKitClicked()
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Save kit as",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getChildFile ("MyKit.asamplerkit"),
            "*.asamplerkit");

        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file == juce::File{}) return;
                processor.saveKitToFile (file.withFileExtension ("asamplerkit"));
            });
    }

    void ASamplerEditor::onLoadKitClicked()
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Load a kit",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
            "*.asamplerkit");

        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file == juce::File{}) return;
                processor.loadKitFromFile (file);
            });
    }

    void ASamplerEditor::onFactoryKitClicked()
    {
        // A plain popup menu rather than a custom picker: JUCE's PopupMenu is
        // keyboard navigable and exposed to UIA as a menu, so it reads properly.
        juce::PopupMenu menu;
        const auto names = ASamplerProcessor::getFactoryKitNames();
        for (int i = 0; i < names.size(); ++i)
            menu.addItem (i + 1, names[i]);

        menu.showMenuAsync (
            juce::PopupMenu::Options().withTargetComponent (factoryKitButton.get()),
            [this] (int result)
            {
                if (result > 0)
                    processor.loadFactoryKit (result - 1);
            });
    }

    void ASamplerEditor::onRelinkClicked()
    {
        const int missing = processor.getMissingSampleCount();
        if (missing == 0)
        {
            announce ("No missing samples to relink.");
            return;
        }

        fileChooser = std::make_unique<juce::FileChooser> (
            "Choose a folder to search for " + juce::String (missing) + " missing samples",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));

        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const auto dir = fc.getResult();
                if (dir == juce::File{}) return;
                processor.relinkMissingSamples (dir);
            });
    }

    void ASamplerEditor::onCopyPadClicked()
    {
        processor.copyPad (processor.getCurrentFocusedPad());
    }

    void ASamplerEditor::onPastePadClicked()
    {
        processor.pastePad (processor.getCurrentFocusedPad(), true);
    }

    void ASamplerEditor::onClearAllClicked()
    {
        // Destructive and wide-reaching, so confirm first. Without sight there
        // is no way to notice 16 pads emptying by accident.
        juce::NativeMessageBox::showOkCancelBox (
            juce::MessageBoxIconType::WarningIcon,
            "Clear all pads",
            "Reset all 16 pads to the default sample? You can undo this.",
            this,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                if (result == 1)
                    processor.clearAllPads();
                else
                    announce ("Clear all pads cancelled.");
            }));
    }

    void ASamplerEditor::onExportMidiClicked()
    {
        const int slices = processor.getCurrentChopCount();

        fileChooser = std::make_unique<juce::FileChooser> (
            "Export slice MIDI",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getChildFile ("ASampler-slices.mid"),
            "*.mid");

        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, slices] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file == juce::File{}) return;
                processor.exportSliceMidi (file.withFileExtension ("mid"), slices);
            });
    }

    void ASamplerEditor::onNormaliseClicked()
    {
        processor.normaliseLayer (processor.getCurrentFocusedPad(),
                                  processor.getCurrentEditLayer());
    }

    void ASamplerEditor::onSnapTransientsClicked()
    {
        processor.snapPadWindowToTransients (processor.getCurrentFocusedPad());
    }

    void ASamplerEditor::onRenderFormantsClicked()
    {
        const int pad = processor.getCurrentFocusedPad();
        announce ("Rendering formant correction for Pad " + juce::String (pad + 1) + "...");
        processor.renderFormantTableNow (pad);

        const int n = processor.getFormantVariantCount (pad);
        announce (n > 0
                    ? ("Pad " + juce::String (pad + 1) + " formant correction ready, "
                       + juce::String (n) + " notes covered.")
                    : ("Pad " + juce::String (pad + 1)
                       + " has no formant correction to build. It needs Keyboard mode "
                         "and Formant Correct switched on."));
    }

    void ASamplerEditor::onRenderStretchClicked()
    {
        const int pad = processor.getCurrentFocusedPad();
        announce ("Rendering Pad " + juce::String (pad + 1) + "...");
        processor.renderStretchNow (pad);
        announce ("Pad " + juce::String (pad + 1)
                  + (processor.isPadRendered (pad)
                         ? " rendered with stretch and formant settings."
                         : " has no stretch or formant settings to apply."));
    }

    // ---- Keyboard pad triggering ----

    int ASamplerEditor::padForKey (const juce::KeyPress& key)
    {
        // Ignore modified presses so Ctrl+Z and friends still reach the host.
        if (key.getModifiers().isAnyModifierKeyDown())
            return -1;

        const auto ch = juce::CharacterFunctions::toLowerCase (
            static_cast<juce::juce_wchar> (key.getKeyCode()));

        for (int row = 0; row < 4; ++row)
        {
            const char* keys = kPadKeyRows[row];
            for (int col = 0; col < 4; ++col)
                if (ch == static_cast<juce::juce_wchar> (keys[col]))
                    return row * 4 + col;
        }
        return -1;
    }

    bool ASamplerEditor::keyPressed (const juce::KeyPress& key)
    {
        const int pad = padForKey (key);
        if (pad < 0)
            return false;   // not ours — let it propagate

        processor.auditionPad (pad);
        return true;
    }

    void ASamplerEditor::onVocalSpeakClicked()
    {
        processor.synthesizeVocalForPad (processor.getCurrentVocalTargetPad(),
                                         vocalEditor.getText());
        // Status (spoken / failed) is announced via the ChangeListener path.
    }

    void ASamplerEditor::onVocalChopClicked()
    {
        processor.synthesizeVocalAndChop (processor.getCurrentVocalTargetPad(),
                                          vocalEditor.getText());
    }

    void ASamplerEditor::onVocalSpreadClicked()
    {
        processor.synthesizeVocalAndSpread (processor.getCurrentVocalTargetPad(),
                                            vocalEditor.getText());
    }

    void ASamplerEditor::refreshVocalLabels()
    {
        const int pad = processor.getCurrentVocalTargetPad() + 1;

        const auto speak  = "Generate vocal to Pad " + juce::String (pad);
        const auto chop   = "Generate and chop across pads (from Pad " + juce::String (pad) + ")";
        const auto spread = "Generate and spread across keyboard (Pad " + juce::String (pad) + ")";

        vocalSpeakButton.setButtonText (speak);
        vocalSpeakButton.setTitle (speak);
        vocalChopButton.setButtonText (chop);
        vocalChopButton.setTitle (chop);
        vocalSpreadButton.setButtonText (spread);
        vocalSpreadButton.setTitle (spread);
    }

    void ASamplerEditor::refreshUtilityLabels()
    {
        const int pad = processor.getCurrentFocusedPad() + 1;

        // Pad-scoped actions say which pad they will hit. "Paste pad" alone
        // gives no way to tell where it is about to land.
        const auto copyText  = "Copy Pad " + juce::String (pad);
        const auto pasteText = "Paste to Pad " + juce::String (pad);
        const auto normText  = "Normalise Pad " + juce::String (pad) + " layer";
        const auto snapText  = "Snap Pad " + juce::String (pad) + " to transients";
        const auto rendText  = "Render Pad " + juce::String (pad) + " stretch";
        const auto fcorText  = "Render Pad " + juce::String (pad) + " formants";

        if (copyPadButton  != nullptr) { copyPadButton ->setButtonText (copyText);  copyPadButton ->setTitle (copyText); }
        if (pastePadButton != nullptr) { pastePadButton->setButtonText (pasteText); pastePadButton->setTitle (pasteText); }
        if (normaliseButton != nullptr) { normaliseButton->setButtonText (normText); normaliseButton->setTitle (normText); }
        if (snapTransientsButton != nullptr) { snapTransientsButton->setButtonText (snapText); snapTransientsButton->setTitle (snapText); }
        if (renderStretchButton != nullptr) { renderStretchButton->setButtonText (rendText); renderStretchButton->setTitle (rendText); }
        if (renderFormantsButton != nullptr) { renderFormantsButton->setButtonText (fcorText); renderFormantsButton->setTitle (fcorText); }

        // Surface unresolved samples on the Relink button itself, so the user
        // discovers the problem by tabbing rather than only from a status
        // message they may have missed.
        if (relinkButton != nullptr)
        {
            const int missing = processor.getMissingSampleCount();
            const auto text = missing > 0
                                ? ("Relink " + juce::String (missing) + " missing sample"
                                   + (missing == 1 ? "..." : "s..."))
                                : juce::String ("Relink samples...");
            relinkButton->setButtonText (text);
            relinkButton->setTitle (text);
        }
    }

    void ASamplerEditor::refreshPadButtonLabels()
    {
        const int chopCount   = processor.getCurrentChopCount();
        const int editLayer   = processor.getCurrentEditLayer();
        const int focusedPad  = processor.getCurrentFocusedPad();
        const auto layerName  = "Layer " + juce::String (editLayer + 1);

        for (int i = 0; i < kNumPads; ++i)
        {
            const int defaultNote = 36 + i;  // matches param default (35 + 1-based)
            const auto noteName   = midiNoteName (defaultNote);
            const auto sampleName = processor.getPadSampleDisplayName (i);

            // Name the QWERTY key that auditions this pad, so the shortcut is
            // discoverable by tabbing rather than only from documentation.
            const auto padKey = juce::String::charToString (
                juce::CharacterFunctions::toUpperCase (
                    static_cast<juce::juce_wchar> (kPadKeyRows[i / 4][i % 4])));

            const auto label = "Pad " + juce::String (i + 1)
                             + (i == focusedPad ? " (focused)" : "")
                             + " (note " + noteName + ", " + layerName + "): Load sample. Currently: "
                             + sampleName;

            loadButtons[i]->setButtonText (label);
            // The accessibility title takes precedence over the visual button
            // text for screen readers when both are set — keep them in sync.
            loadButtons[i]->setTitle (label);

            const auto audLabel = "Play Pad " + juce::String (i + 1) + " (key " + padKey + ")";
            auditionButtons[i]->setButtonText (audLabel);
            auditionButtons[i]->setTitle (audLabel);

            const auto resetLabel = "Pad " + juce::String (i + 1)
                                  + " (" + layerName + "): Reset to default";
            resetButtons[i]->setButtonText (resetLabel);
            resetButtons[i]->setTitle (resetLabel);

            // Say what chop will overwrite, not just what it will do — it
            // rewrites the first N pads, which is easy to miss without sight.
            const auto chopLabel = "Pad " + juce::String (i + 1)
                                 + ": Chop into " + juce::String (chopCount)
                                 + " slices, replacing pads 1 to " + juce::String (chopCount);
            chopButtons[i]->setButtonText (chopLabel);
            chopButtons[i]->setTitle (chopLabel);
        }
    }

    void ASamplerEditor::refreshStatusLabel()
    {
        const auto msg = processor.getLastStatusMessage();
        if (msg.isNotEmpty())
            statusLabel.setText ("Status: " + msg, juce::dontSendNotification);
    }

    void ASamplerEditor::announce (const juce::String& message)
    {
        if (message.isEmpty())
            return;

        // Don't repeat an announcement that is already the most recent thing
        // said. Bursty actions (chop touches every pad, kit load touches every
        // parameter) can fire the change listener many times for one logical
        // event, and a queue of identical speech is something the user has to
        // sit through before they can do anything else.
        if (message == lastAnnounced)
            return;
        lastAnnounced = message;

        // Failures and warnings interrupt; routine confirmations wait their
        // turn. Getting this wrong in either direction is bad: high priority
        // everywhere talks over the user, low priority everywhere means an
        // error scrolls past unheard.
        const bool isProblem = message.containsIgnoreCase ("could not")
                            || message.containsIgnoreCase ("missing")
                            || message.containsIgnoreCase ("failed")
                            || message.containsIgnoreCase ("cannot")
                            || message.containsIgnoreCase ("not found")
                            || message.containsIgnoreCase ("unsupported");

        // Speak the message via the screen reader even if focus didn't move.
        // UIA Notification event on Windows; ignored on platforms without UIA.
        if (auto* handler = getAccessibilityHandler())
            handler->postAnnouncement (
                message,
                isProblem ? juce::AccessibilityHandler::AnnouncementPriority::high
                          : juce::AccessibilityHandler::AnnouncementPriority::medium);
    }
}
