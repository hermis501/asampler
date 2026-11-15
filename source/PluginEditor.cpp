#include "PluginEditor.h"
#include "ClipboardFiles.h"

namespace asampler
{
    namespace
    {
        constexpr int kPadding       = 12;
        constexpr int kGap           = 8;
        constexpr int kHeadingHeight = 32;
        constexpr int kLabelHeight   = 24;
        constexpr int kRowHeight     = 28;
        constexpr int kPadWidth      = 180;
        constexpr int kPadHeight     = 52;
        constexpr int kButtonWidth   = 180;
        constexpr int kStatusHeight  = 72;
        constexpr int kColumns       = 4;
        constexpr int kLeftWidth     = kColumns * kPadWidth + (kColumns - 1) * kGap;   // 744
        constexpr int kWaveHeight    = 180;

       #if JUCE_WINDOWS
        // The Applications (context menu) key, as JUCE's Windows key codes
        // spell a raw virtual key (VK_APPS).
        constexpr int kAppsKeyCode = 0x10000 | 0x5D;
       #endif

        const std::array<float, 6> kScales { 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };

        // MPC-style QWERTY layout: the bottom keyboard row is pads 1-4 and each
        // row above it steps up a bank of four, matching the pad grid.
        constexpr const char* kPadKeyRows[4] = { "zxcv", "asdf", "qwer", "1234" };

       #if JUCE_MAC
        const juce::String kCmd = "Cmd";
       #else
        const juce::String kCmd = "Ctrl";
       #endif

        void setButtonLabel (juce::Button& b, const juce::String& text)
        {
            // The accessible title takes precedence over the visible text, so
            // the two are always set together. Only on change: retitling the
            // focused control makes some screen readers repeat it.
            if (b.getButtonText() != text) b.setButtonText (text);
            if (b.getTitle() != text)      b.setTitle (text);
        }

        juce::String padKeyName (int padIndex)
        {
            const int slot = padIndex % kPadsPerBank;
            return juce::String::charToString (juce::CharacterFunctions::toUpperCase (
                static_cast<juce::juce_wchar> (kPadKeyRows[slot / 4][slot % 4])));
        }

        juce::String bankName (int bank)
        {
            return juce::String::charToString (static_cast<juce::juce_wchar> ('A' + bank));
        }

        juce::String bankRange (int bank)
        {
            return juce::String (bank * kPadsPerBank + 1) + " to " + juce::String ((bank + 1) * kPadsPerBank);
        }

        bool isAudioFile (const juce::File& f)
        {
            return f.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
        }

        // A slice point as the host list reads it ("1.250 s").
        juce::String slicePointText (ASamplerProcessor& proc, int padIndex, bool isEnd, float norm)
        {
            const int p = padIndex + 1;
            if (auto* param = proc.getState().getParameter (isEnd ? param_ids::padEnd (p) : param_ids::padStart (p)))
                return param->getText (param->convertTo0to1 (norm), 0);
            return juce::String (norm, 3);
        }
    }

    // ---- PadButton ----------------------------------------------------------

    bool ASamplerEditor::PadButton::keyPressed (const juce::KeyPress& key)
    {
        if (onKeyFirst && onKeyFirst (key))
            return true;
        if (onArrowKey && onArrowKey (key))
            return true;
        return juce::TextButton::keyPressed (key);   // Space / Enter click
    }

    void ASamplerEditor::PadButton::focusGained (FocusChangeType cause)
    {
        juce::TextButton::focusGained (cause);
        repaint();
        if (onFocus)
            onFocus();
    }

    void ASamplerEditor::PadButton::focusLost (FocusChangeType cause)
    {
        juce::TextButton::focusLost (cause);
        repaint();
    }

    void ASamplerEditor::PadButton::mouseDown (const juce::MouseEvent& e)
    {
        draggedOut = false;
        popupClick = e.mods.isPopupMenu();
        if (popupClick)
        {
            if (onContextMenu)
                onContextMenu();
            return;   // a right-click opens the menu, it doesn't play the pad
        }
        juce::TextButton::mouseDown (e);
    }

    void ASamplerEditor::PadButton::mouseDrag (const juce::MouseEvent& e)
    {
        if (popupClick || draggedOut)
            return;
        if (e.getDistanceFromDragStart() > 8 && onDragOut)
        {
            draggedOut = true;
            setState (juce::Button::buttonNormal);
            onDragOut();
            return;
        }
        juce::TextButton::mouseDrag (e);
    }

    void ASamplerEditor::PadButton::mouseUp (const juce::MouseEvent& e)
    {
        if (popupClick || draggedOut)
        {
            // Neither a right-click nor a drag is a click on the pad.
            popupClick = draggedOut = false;
            setState (juce::Button::buttonNormal);
            return;
        }
        juce::TextButton::mouseUp (e);
    }

    void ASamplerEditor::PadButton::paintButton (juce::Graphics& g, bool isOver, bool isDown)
    {
        using PC = ASamplerProcessor::PadContent;
        auto r = getLocalBounds().toFloat().reduced (1.5f);

        auto base = face.content == PC::Loaded  ? juce::Colour (0xff2c4a66)
                  : face.content == PC::Missing ? juce::Colour (0xff6a3030)
                                                : juce::Colour (0xff2a2e35);
        if (face.muted) base = base.darker (0.5f).withMultipliedSaturation (0.4f);
        if (isDown)      base = base.brighter (0.2f);
        else if (isOver) base = base.brighter (0.08f);

        g.setColour (base);
        g.fillRoundedRectangle (r, 6.0f);

        // Lights up when the pad plays, brighter for a harder hit.
        if (flash > 0.0f)
        {
            g.setColour (juce::Colour (0xffffc860).withAlpha (juce::jlimit (0.0f, 0.7f, flash * 0.7f)));
            g.fillRoundedRectangle (r, 6.0f);
        }

        const bool selected = getToggleState();
        if (hasKeyboardFocus (false))
        {
            g.setColour (juce::Colour (0xff7cc4ff));
            g.drawRoundedRectangle (r.reduced (1.0f), 6.0f, 3.0f);
        }
        else
        {
            g.setColour (selected ? juce::Colours::white : juce::Colours::white.withAlpha (0.12f));
            g.drawRoundedRectangle (r, 6.0f, selected ? 2.0f : 1.0f);
        }

        auto inner = r.reduced (8.0f, 5.0f);
        auto top = inner.removeFromTop (18.0f);
        g.setColour (juce::Colours::white.withAlpha (face.muted ? 0.5f : 0.95f));
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (juce::String (face.number), top, juce::Justification::centredLeft, false);

        // Mute / solo badges, top right.
        auto badges = top;
        auto badge = [&] (const juce::String& t, juce::Colour c)
        {
            auto b = badges.removeFromRight (18.0f).reduced (1.0f);
            g.setColour (c);
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (t, b, juce::Justification::centred, false);
            badges.removeFromRight (2.0f);
        };
        if (face.soloed) badge ("S", juce::Colour (0xffe8c45f));
        if (face.muted)  badge ("M", juce::Colour (0xffe86a6a));

        // The computer key that plays it, bottom right.
        auto bottom = inner.removeFromBottom (16.0f);
        if (face.key.isNotEmpty())
        {
            auto k = bottom.removeFromRight (16.0f);
            g.setColour (juce::Colours::white.withAlpha (0.15f));
            g.fillRoundedRectangle (k, 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.7f));
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (face.key, k, juce::Justification::centred, false);
        }

        g.setColour (face.content == PC::BuiltIn ? juce::Colours::white.withAlpha (0.45f)
                                                 : juce::Colours::white.withAlpha (face.muted ? 0.5f : 0.9f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText (face.name, inner.getUnion (bottom).toNearestInt(), juce::Justification::topLeft, 2, 0.8f);
    }

    // ---- Output meter -------------------------------------------------------

    void ASamplerEditor::OutputMeter::setLevel (float linear)
    {
        // Falls back gently, so a short hit is still seen.
        level = juce::jmax (linear, level * 0.85f);
        if (level < 1.0e-4f) level = 0.0f;
        if (std::abs (level - shown) > 0.002f)
        {
            shown = level;
            repaint();
        }
    }

    void ASamplerEditor::OutputMeter::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff15191e));
        g.fillRoundedRectangle (r, 3.0f);

        const float db = juce::Decibels::gainToDecibels (level, -60.0f);
        const float t  = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        auto bar = r.reduced (2.0f);
        bar = bar.withWidth (bar.getWidth() * t);
        g.setColour (level >= 0.99f ? juce::Colour (0xffe86a6a)
                                    : db > -6.0f ? juce::Colour (0xffe8c45f) : juce::Colour (0xff6ad48a));
        g.fillRoundedRectangle (bar, 2.0f);
    }

    // ---- Construction -------------------------------------------------------

    ASamplerEditor::ASamplerEditor (ASamplerProcessor& proc)
        : juce::AudioProcessorEditor (proc),
          processor (proc),
          lookAndFeel (juce::LookAndFeel_V4::getDarkColourScheme())
    {
        setWantsKeyboardFocus (true);

        // A modest dark look; JUCE's own widgets, only recoloured.
        lookAndFeel.setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (0xff1d2127));
        lookAndFeel.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2b3139));
        lookAndFeel.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff2b3139));
        lookAndFeel.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff5fb0e8));
        lookAndFeel.setColour (juce::Slider::thumbColourId, juce::Colour (0xff7cc4ff));
        setLookAndFeel (&lookAndFeel);

        addAndMakeVisible (content);

        auto setupLabel = [this] (juce::Label& l, const juce::String& text, float size, bool bold)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)));
            l.setJustificationType (juce::Justification::centredLeft);
            l.setAccessible (true);
            l.setTitle (text);
            content.addAndMakeVisible (l);
        };

        const auto version = "ASampler " + ASamplerProcessor::getVersionString();
        setupLabel (heading, version, 20.0f, true);
        heading.setDescription ("Press F1 for keyboard help.");

        setupLabel (gridLabel, "Pads", 14.0f, true);
        addButton (bankButton, [this] { switchBank(); },
                   "Show the other bank of 16 pads. Page Up and Page Down do the same from the grid.");

        for (int i = 0; i < kNumPads; ++i)
        {
            auto pad = std::make_unique<PadButton>();
            pad->setWantsKeyboardFocus (i == selectedPad());   // the grid is one Tab stop
            pad->setClickingTogglesState (false);
            pad->onClick    = [this, i] { selectPad (i, false); processor.auditionPad (i, 0.8f, false); };
            pad->onFocus    = [this, i] { if (selectedPad() != i) selectPad (i, false); };
            pad->onKeyFirst = [this] (const juce::KeyPress& k) { return handleModeKey (k); };
            pad->onArrowKey = [this, i] (const juce::KeyPress& k) { return handleGridArrow (i, k); };
            pad->onContextMenu = [this, i] { selectPad (i, false); showPadMenu (i); };
            pad->onDragOut  = [this, i]
            {
                if (padButtons[(std::size_t) i] != nullptr)
                    startDragging ("pad:" + juce::String (i), padButtons[(std::size_t) i].get(),
                                   juce::ScaledImage(), true);
            };
            pad->setTooltip ("Click to play and select. Right-click for its menu. Drop audio files here "
                             "to load them; drag the pad out to save it as audio.");
            content.addAndMakeVisible (pad.get());
            padButtons[(std::size_t) i] = std::move (pad);
        }

        setupLabel (padActionsLabel, "Selected pad", 14.0f, true);
        addButton (loadButton,     [this] { loadSample(); },
                   "Choose sample files for the selected pad. Choosing several offers one per pad, "
                   "round-robin layers or velocity layers. Files can also be dropped on a pad, or pasted "
                   "with Control V after copying them in Explorer.");
        addButton (prevButton,     [this] { browse (-1); },
                   "Load and play the previous audio file in the same folder.");
        addButton (nextButton,     [this] { browse (+1); },
                   "Load and play the next audio file in the same folder.");
        addButton (tryPrevButton,  [this] { trySample (-1); },
                   "Play the previous file in the folder on this pad, without replacing its sample.");
        addButton (tryNextButton,  [this] { trySample (+1); },
                   "Play the next file in the folder on this pad, without replacing its sample. "
                   "Enter keeps the one you are trying, Escape goes back.");
        addButton (keepTryButton,  [this] { keepTriedSample(); },
                   "Keep the sample being tried on this pad.");
        addButton (playButton,     [this] { playSelected(); });
        addButton (editStartButton, [this] { editSlicePoint (false); },
                   "Place Start by ear. Left and Right move 10 milliseconds, with Control 1, with Shift 100, "
                   "and play from the new point. Enter keeps it, Escape restores it.");
        addButton (editEndButton,   [this] { editSlicePoint (true); },
                   "Place End by ear. Each step plays the sound leading up to the end backwards. "
                   "Enter keeps it, Escape restores it.");
        addButton (trimButton,     [this] { trimSelected(); },
                   "Move Start past leading silence and End to where the sound dies away. Undo restores them.");
        addButton (describeButton, [this] { describeSelected(); },
                   "Speak the sample's length, level, hits, and tempo, key or pitch where it has one.");
        addButton (renameButton,   [this] { renameSelected(); },
                   "Give the pad a name, spoken with it and saved with the project and kits.");
        addButton (noteLearnButton, [this] { showNoteLearnMenu(); },
                   "Set the pad's note, or the low or high note of its range, by playing it on a MIDI keyboard.");
        addButton (applyButton,    [this] { showApplyMenu(); },
                   "Copy one setting, or a whole section, from this pad to all pads or to this bank.");
        addButton (resetButton,    [this] { resetSelected(); },
                   "Put the built-in tone back on the selected pad. Undo restores it.");
        addButton (chopButton,     [this] { chopSelected(); },
                   "Slice the selected pad's sample across the first pads. Undo restores them.");
        addButton (copyButton,     [this] { copySelected(); });
        addButton (pasteButton,    [this] { pasteSelected(); });
        addButton (normaliseButton, [this] { normaliseSelected(); },
                   "Set the layer gain so its peak sits at -1 dB. The audio itself is not changed.");
        addButton (spreadButton,    [this] { spreadSelected(); },
                   "Share the velocity range out evenly between the pad's loaded layers, layer 1 softest.");
        addButton (snapButton,     [this] { snapSelected(); },
                   "Move start and end onto the nearest detected hits.");
        addButton (bounceButton,   [this] { bounceSelected(); },
                   "Render the pad with its envelope, filter and character to a new sample on the next empty pad.");
        addButton (grabButton,     [this] { grabReaperItem(); },
                   "Render the one selected REAPER item, as you hear it, onto this pad. REAPER's project is left as it was.");
        addButton (renderStretchButton,  [this] { renderStretchSelected(); },
                   "Apply time-stretch and formant settings now instead of waiting for the background render.");
        addButton (renderFormantsButton, [this] { renderFormantsSelected(); },
                   "Build the per-note formant correction now.");
        addButton (tuneButton,           [this] { tuneSelected(); },
                   "Detect the sample's pitch and set the pad's Pitch so it plays in tune: on the nearest "
                   "note, or on its root key in Keyboard mode. Undo restores it.");
        addButton (moreButton,           [this] { showPadMenu (selectedPad()); },
                   "Every action for the selected pad, in a menu. Shift F10, the Applications key or a "
                   "right-click on a pad open it too.");

        setupLabel (kitLabel, "Kit", 14.0f, true);
        addButton (undoButton,       [this] { processor.undoLastAction(); });
        addButton (redoButton,       [this] { processor.redoLastAction(); });
        addButton (saveKitButton,    [this] { saveKit(); },
                   "Save the kit: with sample paths, with copies of the samples beside it, or as a "
                   "template of settings only.");
        addButton (loadKitButton,    [this] { loadKit(); });
        addButton (factoryKitButton, [this] { chooseFactoryKit(); },
                   "Load a built-in starting point. Starter Drums also puts eight built-in sounds on pads 1 to 8; "
                   "the others change settings only.");
        addButton (relinkButton,     [this] { relink(); },
                   "Search a folder for samples this project could not find.");
        addButton (clearAllButton,   [this] { clearAll(); },
                   "Reset every pad to the default sample. Undo restores them.");
        addButton (exportMidiButton, [this] { exportMidi(); },
                   "Write a MIDI file with one note per slice, to import into your project.");
        addButton (helpButton,       [this] { showHelp(); });
        addButton (sizeButton,       [this] { showSizeMenu(); },
                   "Make the window bigger or smaller. Dragging its corner works too.");

        // Used less often: reachable from the pad menu (and their shortcuts),
        // not as buttons, so Tab gets through the window quickly.
        for (auto* b : { tryPrevButton.get(), tryNextButton.get(), keepTryButton.get(), trimButton.get(),
                         renameButton.get(), noteLearnButton.get(), applyButton.get(), resetButton.get(),
                         normaliseButton.get(), spreadButton.get(), snapButton.get(), grabButton.get(),
                         renderStretchButton.get(), renderFormantsButton.get(), tuneButton.get() })
            b->setVisible (false);

        setupLabel (statusLabel, "Ready. Press F1 for keyboard help.", 14.0f, false);
        statusLabel.setTitle ("Status");
        statusLabel.setJustificationType (juce::Justification::topLeft);
        statusLabel.setMinimumHorizontalScale (1.0f);

        settingsPanel = std::make_unique<ParameterPanel> (processor);
        settingsPanel->setTitle ("Pad settings");
        settingsPanel->onKeyFirst = [this] (const juce::KeyPress& k) { return handleModeKey (k); };
        content.addAndMakeVisible (*settingsPanel);

        browser = std::make_unique<SampleBrowser> (processor);
        browser->setTitle ("Sample browser");
        browser->onKeyFirst = [this] (const juce::KeyPress& k) { return handleModeKey (k); };
        browser->onLoad = [this] (const juce::File& f)
        {
            processor.loadSampleForPadLayerAsync (selectedPad(), processor.getCurrentEditLayer(), f);
        };
        browser->onFolderChanged = [this] (const juce::File& f)
        {
            uiSettings->get().setValue ("browserFolder", f.getFullPathName());
        };
        content.addChildComponent (*browser);

        addButton (browserButton, [this] { showBrowser (! browserShown); },
                   "Swap the pad settings for a list of the samples in a folder, and back.");

        waveform = std::make_unique<WaveformView> (processor);
        content.addAndMakeVisible (*waveform);
        content.addAndMakeVisible (meter);

        for (int i = 0; i < kNumPads; ++i)
            lastHitCount[(std::size_t) i] = processor.getPadHitCount (i);

        // Tab order: what the user reaches, in the order the header describes,
        // regardless of where it sits on screen.
        {
            int order = 1;
            auto next = [&order] (juce::Component& c) { c.setExplicitFocusOrder (order++); };
            next (heading);
            next (gridLabel);
            next (*bankButton);
            for (auto& pad : padButtons)
                next (*pad);
            next (*settingsPanel);
            next (*browser);         // shown instead of the settings, never with them
            next (*browserButton);
            next (padActionsLabel);
            for (auto* b : { loadButton.get(), prevButton.get(), nextButton.get(), playButton.get(),
                             editStartButton.get(), editEndButton.get(), describeButton.get(),
                             chopButton.get(), copyButton.get(), pasteButton.get(), bounceButton.get(),
                             moreButton.get(),
                             // hidden, in the pad menu:
                             tryPrevButton.get(), tryNextButton.get(), keepTryButton.get(), trimButton.get(),
                             renameButton.get(), noteLearnButton.get(), applyButton.get(), resetButton.get(),
                             normaliseButton.get(), spreadButton.get(), snapButton.get(), grabButton.get(),
                             renderStretchButton.get(), renderFormantsButton.get(), tuneButton.get() })
                next (*b);
            next (kitLabel);
            for (auto* b : { undoButton.get(), redoButton.get(), saveKitButton.get(), loadKitButton.get(),
                             factoryKitButton.get(), relinkButton.get(), clearAllButton.get(),
                             exportMidiButton.get(), helpButton.get(), sizeButton.get() })
                next (*b);
            next (statusLabel);
        }

        refreshLabels();
        processor.addChangeListener (this);

        // Labels follow parameter changes made anywhere (host list,
        // automation, undo), so they are polled rather than wired to each of
        // the ~2,900 parameters.
        startTimer (250);

        // The window keeps its shape when resized; the scale last chosen on
        // this computer is where it starts.
        setResizable (true, true);
        setResizeLimits (juce::roundToInt (kDesignWidth * 0.6), juce::roundToInt (kDesignHeight * 0.6),
                         juce::roundToInt (kDesignWidth * 2.5), juce::roundToInt (kDesignHeight * 2.5));
        if (auto* c = getConstrainer())
            c->setFixedAspectRatio ((double) kDesignWidth / (double) kDesignHeight);

        const auto stored = (float) uiSettings->get().getDoubleValue ("scale", 1.0);
        setUiScale (std::isfinite (stored) ? stored : 1.0f, false);
        persistScale = true;   // from here on, a resize by the user is remembered
    }

    ASamplerEditor::~ASamplerEditor()
    {
        // Nothing may stay hanging on a held key once the window is gone.
        for (const auto& [code, note] : heldPadKeys)
            processor.queueLiveNote (note, false);
        processor.removeChangeListener (this);
        if (browserShown)
            processor.stopFilePreview();
        setLookAndFeel (nullptr);
    }

    juce::TextButton& ASamplerEditor::addButton (std::unique_ptr<juce::TextButton>& slot,
                                                 std::function<void()> onClick,
                                                 const juce::String& description)
    {
        slot = std::make_unique<juce::TextButton>();
        slot->setWantsKeyboardFocus (true);
        if (description.isNotEmpty())
        {
            slot->setDescription (description);
            // The same words on mouse hover. For a button JUCE keeps the
            // tooltip out of what the screen reader reads, so nothing is
            // spoken twice. (Sliders and combo boxes would read it as help
            // text, which is why they don't get one.)
            slot->setTooltip (description);
        }
        slot->onClick = std::move (onClick);
        content.addAndMakeVisible (slot.get());
        return *slot;
    }

    // ---- Layout -------------------------------------------------------------

    void ASamplerEditor::paint (juce::Graphics& g)
    {
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    }

    void ASamplerEditor::setUiScale (float scale, bool persist)
    {
        scale = juce::jlimit (0.6f, 2.5f, scale);
        const bool wasPersisting = persistScale;
        persistScale = persist;
        setSize (juce::roundToInt (kDesignWidth * scale), juce::roundToInt (kDesignHeight * scale));
        resized();   // the same size again still has to apply the scale
        persistScale = wasPersisting;
    }

    void ASamplerEditor::resized()
    {
        const float scale = getWidth() > 0 ? (float) getWidth() / (float) kDesignWidth : 1.0f;
        uiScale = scale;
        content.setBounds (0, 0, kDesignWidth, kDesignHeight);
        content.setTransform (juce::AffineTransform::scale (scale));
        layoutContent();

        if (persistScale)
            uiSettings->get().setValue ("scale", (double) scale);
        if (sizeButton != nullptr)
            setButtonLabel (*sizeButton, "Window size... (" + juce::String (juce::roundToInt (scale * 100.0f)) + "%)");
    }

    void ASamplerEditor::layoutContent()
    {
        auto area = juce::Rectangle<int> (0, 0, kDesignWidth, kDesignHeight).reduced (kPadding);

        // Right column: the waveform, then the settings or the browser.
        {
            auto right = area.removeFromRight (area.getWidth() - kLeftWidth - kPadding);
            waveform->setBounds (right.removeFromTop (kWaveHeight));
            right.removeFromTop (kGap);
            browserButton->setBounds (right.removeFromBottom (kRowHeight).removeFromLeft (300));
            right.removeFromBottom (kGap);
            settingsPanel->setBounds (right);
            browser->setBounds (right);
        }
        area = area.withWidth (kLeftWidth);

        {
            auto line = area.removeFromTop (kHeadingHeight);
            meter.setBounds (line.removeFromRight (kButtonWidth).withSizeKeepingCentre (kButtonWidth, 10));
            heading.setBounds (line);
        }
        area.removeFromTop (kGap);

        {
            auto line = area.removeFromTop (kLabelHeight);
            bankButton->setBounds (line.removeFromRight (kButtonWidth));
            gridLabel.setBounds (line);
        }
        area.removeFromTop (kGap);

        // Pad 1 bottom left, Pad 16 top right, like the hardware. Both banks
        // share the same 16 positions; only the current bank is visible.
        for (int row = 0; row < 4; ++row)
        {
            auto line = area.removeFromTop (kPadHeight);
            const int rowOfFour = 3 - row;
            for (int col = 0; col < kColumns; ++col)
            {
                const auto bounds = line.removeFromLeft (kPadWidth);
                for (int bank = 0; bank * kPadsPerBank < kNumPads; ++bank)
                    padButtons[(std::size_t) (bank * kPadsPerBank + rowOfFour * 4 + col)]->setBounds (bounds);
                line.removeFromLeft (kGap);
            }
            area.removeFromTop (kGap);
        }
        area.removeFromTop (kGap);

        auto flow = [&area] (juce::Label& label, std::initializer_list<juce::TextButton*> buttons)
        {
            label.setBounds (area.removeFromTop (kLabelHeight));
            auto row = area.removeFromTop (kRowHeight);
            for (auto* b : buttons)
            {
                if (! b->isVisible())
                    continue;
                if (row.getWidth() < kButtonWidth)
                {
                    area.removeFromTop (kGap);
                    row = area.removeFromTop (kRowHeight);
                }
                b->setBounds (row.removeFromLeft (kButtonWidth));
                row.removeFromLeft (kGap);
            }
            area.removeFromTop (kGap);
        };

        flow (padActionsLabel, { loadButton.get(), prevButton.get(), nextButton.get(), playButton.get(),
                                 editStartButton.get(), editEndButton.get(), describeButton.get(), chopButton.get(),
                                 copyButton.get(), pasteButton.get(), bounceButton.get(), moreButton.get() });

        flow (kitLabel, { undoButton.get(), redoButton.get(), saveKitButton.get(), loadKitButton.get(),
                          factoryKitButton.get(), relinkButton.get(), clearAllButton.get(),
                          exportMidiButton.get(), helpButton.get(), sizeButton.get() });

        statusLabel.setBounds (area);
    }

    void ASamplerEditor::visibilityChanged()
    {
        // Create the announcement window once we are on screen, a moment ahead
        // of the first announcement, so the screen reader has already seen it.
        if (! isShowing())
        {
            for (const auto& [code, note] : heldPadKeys)
                processor.queueLiveNote (note, false);
            heldPadKeys.clear();
        }

        if (isShowing())
        {
            juce::Component::SafePointer<ASamplerEditor> safe (this);
            juce::Timer::callAfterDelay (300, [safe]
            {
                if (safe != nullptr)
                    safe->announcer.prepare();
            });
        }
    }

    void ASamplerEditor::timerCallback()
    {
        refreshLabels();
    }

    void ASamplerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        refreshLabels();
        refreshStatusLabel();
        announce (processor.getLastStatusMessage());
    }

    // ---- Pad grid -----------------------------------------------------------

    void ASamplerEditor::selectPad (int padIndex, bool moveFocus)
    {
        if (! juce::isPositiveAndBelow (padIndex, kNumPads))
            return;

        processor.setFocusedPad (padIndex);

        // Labels first: they show the selected pad's bank, and a hidden
        // button cannot take focus.
        refreshLabels();

        auto& target = *padButtons[(std::size_t) padIndex];
        target.setWantsKeyboardFocus (true);
        if (moveFocus)
            target.grabKeyboardFocus();

        // Only the selected pad stays in the Tab order.
        for (int i = 0; i < kNumPads; ++i)
            if (i != padIndex)
                padButtons[(std::size_t) i]->setWantsKeyboardFocus (false);
    }

    bool ASamplerEditor::handleGridArrow (int padIndex, const juce::KeyPress& key)
    {
        if (key.getModifiers().isAnyModifierKeyDown())
            return false;

        const int col = padIndex % kColumns;
        int next = -1;

        // Up means higher pad numbers: Pad 1 is at the bottom. Up and down run
        // straight through the banks (up from Pad 13 is Pad 17), so there is
        // no dead end to discover by bumping into it.
        if      (key == juce::KeyPress::upKey)    next = padIndex + kColumns < kNumPads ? padIndex + kColumns : -1;
        else if (key == juce::KeyPress::downKey)  next = padIndex - kColumns >= 0 ? padIndex - kColumns : -1;
        else if (key == juce::KeyPress::rightKey) next = col < kColumns - 1 ? padIndex + 1 : -1;
        else if (key == juce::KeyPress::leftKey)  next = col > 0 ? padIndex - 1 : -1;
        else if (key == juce::KeyPress::homeKey)  next = 0;
        else if (key == juce::KeyPress::endKey)   next = kNumPads - 1;
        else if (key == juce::KeyPress::pageUpKey)
            next = padIndex + kPadsPerBank < kNumPads ? padIndex + kPadsPerBank : -1;
        else if (key == juce::KeyPress::pageDownKey)
            next = padIndex - kPadsPerBank >= 0 ? padIndex - kPadsPerBank : -1;
        else return false;

        // At an edge the key is consumed but nothing moves, as in any grid.
        if (next >= 0 && next != padIndex)
            selectPad (next, true);
        return true;
    }

    // ---- Keyboard -----------------------------------------------------------

    int ASamplerEditor::padForKey (const juce::KeyPress& key, int bank)
    {
        if (key.getModifiers().isAnyModifierKeyDown())
            return -1;

        const auto ch = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                if (ch == static_cast<juce::juce_wchar> (kPadKeyRows[row][col]))
                    return bank * kPadsPerBank + row * 4 + col;
        return -1;
    }

    int ASamplerEditor::padForKeyCode (int keyCode, int bank)
    {
        // With Shift held the text character is "!" rather than "1", so the
        // key code is what identifies the key.
        const auto ch = juce::CharacterFunctions::toLowerCase (static_cast<juce::juce_wchar> (keyCode));
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                if (ch == static_cast<juce::juce_wchar> (kPadKeyRows[row][col]))
                    return bank * kPadsPerBank + row * 4 + col;
        return -1;
    }

    void ASamplerEditor::switchBank()
    {
        // Same position in the other bank, so Pad 5 becomes Pad 21.
        const int banks = kNumPads / kPadsPerBank;
        const int next  = ((currentBank() + 1) % banks) * kPadsPerBank + selectedPad() % kPadsPerBank;
        selectPad (next, true);
    }

    bool ASamplerEditor::keyPressed (const juce::KeyPress& key)
    {
        if (handleModeKey (key))
            return true;

        const auto mods = key.getModifiers();
        const auto code = juce::CharacterFunctions::toLowerCase (static_cast<juce::juce_wchar> (key.getKeyCode()));

        if (mods.isCommandDown() && ! mods.isAltDown())
        {
            if (code == 'z' && ! mods.isShiftDown()) { processor.undoLastAction(); return true; }
            if (code == 'y' || (code == 'z' && mods.isShiftDown())) { processor.redoLastAction(); return true; }

            if (! mods.isShiftDown())
            {
                // Ctrl + digits picks a pad by number; it is selected when
                // Ctrl is let go, so "1" then "2" reaches Pad 12.
                if (code >= '0' && code <= '9')
                {
                    jumpDigits << juce::String::charToString (code);
                    return true;
                }

                switch (code)
                {
                    case 's': saveKit();                       return true;
                    case 'o': loadKit();                       return true;
                    case 'v': pasteFiles();                    return true;
                    case 'r': processor.toggleReaperRecord();  return true;
                    case 'g': grabReaperItem();                return true;
                    case 'b': showBrowser (! browserShown);    return true;
                    default: break;
                }
            }
            return false;   // leave other shortcuts to the host
        }

        if (key == juce::KeyPress::F1Key) { showHelp(); return true; }
        if (key.getKeyCode() == juce::KeyPress::F10Key && mods.isShiftDown()
            && ! mods.isCommandDown() && ! mods.isAltDown())
        {
            showPadMenu (selectedPad());
            return true;
        }
        if (key == juce::KeyPress::F2Key) { renameSelected(); return true; }

        if (mods.isShiftDown() && ! mods.isCommandDown() && ! mods.isAltDown())
        {
            if (code == 'n') { trySample (+1); return true; }
            if (code == 'p') { trySample (-1); return true; }

            // Shift + a pad key says what the pad holds, without playing it.
            if (const int pad = padForKeyCode (key.getKeyCode(), currentBank()); pad >= 0)
            {
                speakPad (pad);
                return true;
            }
        }

        if (! mods.isAnyModifierKeyDown())
        {
            switch (juce::CharacterFunctions::toLowerCase (key.getTextCharacter()))
            {
                case 'l': loadSample();             return true;
                case 'n': browse (+1);              return true;
                case 'p': browse (-1);              return true;
                case 'i': describeSelected();       return true;
                case 'b': bounceSelected();         return true;
                case 'u': tuneSelected();           return true;
                case 't': editSlicePoint (false);   return true;
                case 'o': editSlicePoint (true);    return true;
                case 'k': processor.armNoteLearn (selectedPad(), ASamplerProcessor::NoteLearnTarget::Note);
                          return true;
                default: break;
            }

            if (key == juce::KeyPress::spaceKey) { playSelected(); return true; }
        }

        if (! mods.isAnyModifierKeyDown()
            && (key == juce::KeyPress::pageUpKey || key == juce::KeyPress::pageDownKey))
        {
            // From anywhere in the editor, not only the grid.
            const int target = selectedPad() + (key == juce::KeyPress::pageUpKey ? kPadsPerBank : -kPadsPerBank);
            if (juce::isPositiveAndBelow (target, kNumPads))
                selectPad (target, true);
            return true;
        }

        const int pad = padForKey (key, currentBank());
        if (pad < 0)
            return false;   // not ours: let the host have it

        // Held keys play like a keyboard: note-on now, note-off on release
        // (keyStateChanged). Auto-repeat is ignored, so holding a key doesn't
        // machine-gun the pad.
        const int keyCode = key.getKeyCode();
        if (heldPadKeys.count (keyCode) == 0)
        {
            const int note = processor.getPadPlayNote (pad);
            heldPadKeys[keyCode] = note;
            processor.queueLiveNote (note, true);
        }
        return true;
    }

    bool ASamplerEditor::keyStateChanged (bool isKeyDown)
    {
        juce::ignoreUnused (isKeyDown);

       #if JUCE_WINDOWS
        // The Applications key reaches JUCE only as a key-state change.
        const bool apps = juce::KeyPress::isKeyCurrentlyDown (kAppsKeyCode);
        if (apps && ! appsKeyDown)
        {
            appsKeyDown = true;
            showPadMenu (selectedPad());
            return true;
        }
        appsKeyDown = apps;
       #endif

        for (auto it = heldPadKeys.begin(); it != heldPadKeys.end();)
        {
            if (! juce::KeyPress::isKeyCurrentlyDown (it->first))
            {
                processor.queueLiveNote (it->second, false);
                it = heldPadKeys.erase (it);
            }
            else
            {
                ++it;
            }
        }
        return false;   // the host may track keys too
    }

    void ASamplerEditor::modifierKeysChanged (const juce::ModifierKeys& mods)
    {
        if (! mods.isCommandDown() && jumpDigits.isNotEmpty())
        {
            const int number = jumpDigits.getIntValue();
            jumpDigits.clear();
            if (number >= 1 && number <= kNumPads)
            {
                selectPad (number - 1, true);
                lastAnnounced.clear();
                announce (padTitle (number - 1));
            }
            else
            {
                announce ("No pad " + juce::String (number) + ", there are " + juce::String (kNumPads));
            }
        }
        juce::AudioProcessorEditor::modifierKeysChanged (mods);
    }

    // ---- Edit modes -----------------------------------------------------------

    bool ASamplerEditor::handleModeKey (const juce::KeyPress& key)
    {
        if (key == juce::KeyPress::escapeKey && processor.isNoteLearnArmed())
        {
            processor.cancelNoteLearn();
            return true;
        }

        if (mode == Mode::TrySample)
        {
            if (key == juce::KeyPress::returnKey)  { keepTriedSample(); return true; }
            if (key == juce::KeyPress::escapeKey)
            {
                processor.cancelPreview (modePad);
                leaveMode();
                return true;
            }
            return false;
        }

        if (mode != Mode::EditStart && mode != Mode::EditEnd)
            return false;

        const bool isEnd = mode == Mode::EditEnd;
        const auto what  = juce::String (isEnd ? "End" : "Start");

        if (key == juce::KeyPress::returnKey)
        {
            processor.stopPreview();
            processor.setSlicePoint (modePad, isEnd, scrubValue);   // announces
            leaveMode();
            return true;
        }

        if (key == juce::KeyPress::escapeKey)
        {
            processor.stopPreview();
            const float original = processor.getSlicePoint (modePad, isEnd);
            leaveMode();
            lastAnnounced.clear();
            announce (what + " unchanged, " + slicePointText (processor, modePad, isEnd, original));
            return true;
        }

        if (key == juce::KeyPress::spaceKey)
        {
            processor.previewSlicePoint (modePad, isEnd, scrubValue);
            return true;
        }

        const auto mods = key.getModifiers();
        const int  kc   = key.getKeyCode();
        float next = scrubValue;

        if (kc == juce::KeyPress::leftKey || kc == juce::KeyPress::rightKey
            || kc == juce::KeyPress::downKey || kc == juce::KeyPress::upKey)
        {
            const double ms = mods.isCommandDown() ? 1.0 : (mods.isShiftDown() ? 100.0 : 10.0);
            const double seconds = processor.getPadReferenceSeconds (modePad);
            const float  step = seconds > 0.0 ? (float) (ms * 0.001 / seconds) : 0.01f;
            const bool   forward = kc == juce::KeyPress::rightKey || kc == juce::KeyPress::upKey;
            next = juce::jlimit (0.0f, 1.0f, scrubValue + (forward ? step : -step));
        }
        else if (kc == juce::KeyPress::homeKey) next = 0.0f;
        else if (kc == juce::KeyPress::endKey)  next = 1.0f;
        else
            return false;

        scrubValue = next;
        processor.previewSlicePoint (modePad, isEnd, scrubValue);
        lastAnnounced.clear();
        announce (slicePointText (processor, modePad, isEnd, scrubValue));
        return true;
    }

    void ASamplerEditor::leaveMode()
    {
        mode = Mode::Normal;
        refreshLabels();
    }

    void ASamplerEditor::editSlicePoint (bool isEnd)
    {
        const auto wanted = isEnd ? Mode::EditEnd : Mode::EditStart;
        if (mode == wanted)
        {
            // The button again (or T / O again) keeps the point, like Enter.
            handleModeKey (juce::KeyPress (juce::KeyPress::returnKey));
            return;
        }

        if (mode == Mode::TrySample)
            processor.cancelPreview (modePad);

        mode       = wanted;
        modePad    = selectedPad();
        scrubValue = processor.getSlicePoint (modePad, isEnd);
        processor.previewSlicePoint (modePad, isEnd, scrubValue);
        refreshLabels();

        lastAnnounced.clear();
        announce ("Editing " + juce::String (isEnd ? "End" : "Start") + " of Pad " + juce::String (modePad + 1)
                  + ", " + slicePointText (processor, modePad, isEnd, scrubValue)
                  + ". Left and Right move 10 milliseconds, with Control 1, with Shift 100. "
                    "Enter keeps, Escape restores.");
    }

    void ASamplerEditor::trySample (int direction)
    {
        if (mode == Mode::EditStart || mode == Mode::EditEnd)
        {
            processor.stopPreview();
            mode = Mode::Normal;
        }

        const int pad = selectedPad();
        if (mode == Mode::TrySample && modePad != pad)
            processor.cancelPreview (modePad);

        const auto result = processor.previewSample (pad, direction);
        if (result == ASamplerProcessor::BrowseResult::Started
            || result == ASamplerProcessor::BrowseResult::Success)
        {
            mode    = Mode::TrySample;
            modePad = pad;
        }
        refreshLabels();
    }

    void ASamplerEditor::keepTriedSample()
    {
        processor.keepPreview (mode == Mode::TrySample ? modePad : selectedPad());
        leaveMode();
    }

    // ---- Pad actions ----------------------------------------------------------

    void ASamplerEditor::loadFiles (int padIndex, const juce::Array<juce::File>& filesIn, int forcedMode)
    {
        juce::Array<juce::File> files;
        for (const auto& f : filesIn)
            if (f.existsAsFile() && isAudioFile (f))
                files.add (f);

        if (files.isEmpty())
        {
            announce ("No audio files to load");
            return;
        }

        if (files.size() == 1)
        {
            processor.loadSampleForPadLayerAsync (padIndex, processor.getCurrentEditLayer(), files[0]);
            return;
        }

        using M = ASamplerProcessor::MultiLoadMode;
        if (forcedMode >= 0)
        {
            processor.loadSamples (padIndex, files, static_cast<M> (forcedMode));
            return;
        }

        // Several files: say where each choice puts them before anything is
        // overwritten.
        const int n = files.size();
        const int lastPad = juce::jmin (kNumPads, padIndex + n);
        const auto layers = n > kNumLayers ? " (the first " + juce::String (kNumLayers) + ")" : juce::String();

        juce::PopupMenu menu;
        menu.addSectionHeader ("Load " + juce::String (n) + " files");
        menu.addItem (1, "One per pad, replacing Pads " + juce::String (padIndex + 1) + " to " + juce::String (lastPad));
        menu.addItem (2, "Round-robin layers on Pad " + juce::String (padIndex + 1) + layers);
        menu.addItem (3, "Velocity layers on Pad " + juce::String (padIndex + 1) + ", softest first" + layers);

        juce::Component::SafePointer<ASamplerEditor> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (padButtons[(std::size_t) padIndex].get()),
            [safe, padIndex, files] (int result)
            {
                if (safe == nullptr) return;
                if (result <= 0) { safe->announce ("Load cancelled"); return; }
                safe->processor.loadSamples (padIndex, files, static_cast<M> (result - 1));
            });
    }

    bool ASamplerEditor::isInterestedInFileDrag (const juce::StringArray& files)
    {
        for (const auto& f : files)
            if (isAudioFile (juce::File (f)))
                return true;
        return false;
    }

    void ASamplerEditor::filesDropped (const juce::StringArray& paths, int x, int y)
    {
        // Onto the pad under the drop, else the selected pad. The pads live
        // in the scaled content, so the point is converted first.
        int pad = selectedPad();
        const auto where = content.getLocalPoint (this, juce::Point<int> (x, y));
        for (int i = 0; i < kNumPads; ++i)
            if (padButtons[(std::size_t) i]->isVisible() && padButtons[(std::size_t) i]->getBounds().contains (where))
                pad = i;

        selectPad (pad, false);
        juce::Array<juce::File> files;
        for (const auto& p : paths)
            files.add (juce::File (p));
        loadFiles (pad, files);
    }

    void ASamplerEditor::pasteFiles()
    {
        const auto files = getFilesFromClipboard();
        if (files.isEmpty())
        {
            announce ("No files on the clipboard. Copy audio files in Explorer first.");
            return;
        }
        loadFiles (selectedPad(), files);
    }

    void ASamplerEditor::speakPad (int padIndex)
    {
        lastAnnounced.clear();
        announce (padTitle (padIndex));
    }

    void ASamplerEditor::renameSelected()
    {
        const int pad = selectedPad();
        auto* window = new juce::AlertWindow ("Rename Pad " + juce::String (pad + 1),
                                              "Name for Pad " + juce::String (pad + 1) + ". Leave it empty to clear the name.",
                                              juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor ("name", processor.getPadName (pad), "Name");
        window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<ASamplerEditor> safe (this);
        window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window, pad] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1)
                safe->processor.setPadName (pad, window->getTextEditorContents ("name"));
            else
                safe->announce ("Rename cancelled");
        }), true);

        if (auto* editor = window->getTextEditor ("name"))
        {
            editor->selectAll();
            editor->grabKeyboardFocus();
        }
    }

    void ASamplerEditor::showApplyMenu()
    {
        const int pad  = selectedPad();
        const int bank = currentBank();

        juce::StringArray sections;
        for (int i = 0; i < ASamplerProcessor::getNumPadControls(); ++i)
            sections.addIfNotAlreadyThere (ASamplerProcessor::getPadControlSection (i));

        // Ids: scope * 100000, plus 1 + control index, or 50000 + section index.
        auto scopeMenu = [&] (int scope)
        {
            juce::PopupMenu menu;
            for (int s = 0; s < sections.size(); ++s)
            {
                juce::PopupMenu sectionMenu;
                sectionMenu.addItem (scope * 100000 + 50000 + s, "Whole " + sections[s] + " section");
                sectionMenu.addSeparator();
                for (int i = 0; i < ASamplerProcessor::getNumPadControls(); ++i)
                    if (ASamplerProcessor::getPadControlSection (i) == sections[s])
                        sectionMenu.addItem (scope * 100000 + 1 + i, ASamplerProcessor::getPadControlName (i) + ": "
                                                                 + processor.getPadControlValueText (pad, i));
                menu.addSubMenu (sections[s], sectionMenu);
            }
            return menu;
        };

        juce::PopupMenu menu;
        menu.addSectionHeader ("Apply Pad " + juce::String (pad + 1) + " settings");
        menu.addSubMenu ("To all " + juce::String (kNumPads) + " pads", scopeMenu (0));
        menu.addSubMenu ("To this bank, pads " + bankRange (bank), scopeMenu (1));

        juce::Component::SafePointer<ASamplerEditor> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuTarget (applyButton.get())),
            [safe, pad, bank, sections] (int result)
            {
                if (safe == nullptr || result <= 0) return;
                const int scope = result / 100000;
                const int item  = result % 100000;
                const int first = scope == 0 ? 0 : bank * kPadsPerBank;
                const int last  = scope == 0 ? kNumPads - 1 : first + kPadsPerBank - 1;
                if (item >= 50000)
                    safe->processor.applySectionToPads (pad, sections[item - 50000], first, last);
                else
                    safe->processor.applyControlToPads (pad, item - 1, first, last);
            });
    }

    void ASamplerEditor::showNoteLearnMenu()
    {
        const int pad = selectedPad();
        juce::PopupMenu menu;
        menu.addSectionHeader ("Learn from the next MIDI note");
        menu.addItem (1, "Note (the root in Keyboard mode) (K)");
        menu.addItem (2, "Low Note");
        menu.addItem (3, "High Note");

        juce::Component::SafePointer<ASamplerEditor> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuTarget (noteLearnButton.get())),
            [safe, pad] (int result)
            {
                if (safe == nullptr || result <= 0) return;
                safe->processor.armNoteLearn (pad, static_cast<ASamplerProcessor::NoteLearnTarget> (result - 1));
            });
    }

    void ASamplerEditor::trimSelected()    { processor.trimSilence (selectedPad()); }
    void ASamplerEditor::spreadSelected()  { processor.spreadVelocityLayers (selectedPad()); }
    void ASamplerEditor::grabReaperItem()  { processor.grabReaperItem (selectedPad()); }

    void ASamplerEditor::loadSample()
    {
        const int pad = selectedPad();

        // Start in the folder of the pad's current sample, if it has one, so
        // swapping a kick for its neighbour is one step.
        auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        if (const auto current = processor.getPadSampleFile (pad); current.getParentDirectory().isDirectory())
            start = current;

        fileChooser = std::make_unique<juce::FileChooser> (
            "Choose samples for Pad " + juce::String (pad + 1), start,
            "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::canSelectMultipleItems,
            [this, pad] (const juce::FileChooser& fc)
            {
                const auto files = fc.getResults();
                if (files.isEmpty())
                    return;   // cancelled: nothing to announce
                loadFiles (pad, files);
            });
    }

    void ASamplerEditor::browse (int direction)    { processor.browseSample (selectedPad(), direction); }
    void ASamplerEditor::playSelected()            { processor.auditionPad (selectedPad(), 0.8f, false); }
    void ASamplerEditor::describeSelected()        { processor.describePad (selectedPad()); }
    void ASamplerEditor::resetSelected()           { processor.resetPadToDefault (selectedPad()); }
    void ASamplerEditor::chopSelected()            { processor.chopAcrossPads (selectedPad()); }
    void ASamplerEditor::copySelected()            { processor.copyPad (selectedPad()); }
    void ASamplerEditor::pasteSelected()           { processor.pastePad (selectedPad(), true); }
    void ASamplerEditor::snapSelected()            { processor.snapPadWindowToTransients (selectedPad()); }
    void ASamplerEditor::bounceSelected()          { processor.bouncePad (selectedPad()); }
    void ASamplerEditor::tuneSelected()            { processor.tunePad (selectedPad()); }

    void ASamplerEditor::normaliseSelected()
    {
        processor.normaliseLayer (selectedPad(), processor.getCurrentEditLayer());
    }

    void ASamplerEditor::renderStretchSelected()
    {
        const int pad = selectedPad();
        announce ("Rendering Pad " + juce::String (pad + 1) + "...");
        processor.renderStretchNow (pad);
        announce ("Pad " + juce::String (pad + 1)
                  + (processor.isPadRendered (pad) ? " rendered with stretch and formant settings."
                                                   : " has no stretch or formant settings to apply."));
    }

    void ASamplerEditor::renderFormantsSelected()
    {
        const int pad = selectedPad();
        announce ("Rendering formant correction for Pad " + juce::String (pad + 1) + "...");
        processor.renderFormantTableNow (pad);

        const int n = processor.getFormantVariantCount (pad);
        announce (n > 0 ? "Pad " + juce::String (pad + 1) + " formant correction ready, "
                              + juce::String (n) + " notes covered."
                        : "Pad " + juce::String (pad + 1)
                              + " has no formant correction to build. It needs Keyboard mode "
                                "and Formant Correct switched on.");
    }

    // ---- Kit actions ----------------------------------------------------------

    void ASamplerEditor::saveKit()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Save kit");
        menu.addItem (1, "Kit with sample paths");
        menu.addItem (2, "Kit with copies of its samples, in a folder beside it");
        menu.addItem (3, "Template: settings only, no samples");

        juce::Component::SafePointer<ASamplerEditor> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (saveKitButton.get()),
            [safe] (int result)
            {
                if (safe == nullptr || result <= 0) return;
                const auto kitMode = static_cast<ASamplerProcessor::KitSaveMode> (result - 1);

                safe->fileChooser = std::make_unique<juce::FileChooser> (
                    result == 3 ? "Save template as" : "Save kit as",
                    juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                        .getChildFile (result == 3 ? "MyTemplate.asamplerkit" : "MyKit.asamplerkit"),
                    "*.asamplerkit");

                safe->fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                                    | juce::FileBrowserComponent::canSelectFiles
                                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                    [safe, kitMode] (const juce::FileChooser& fc)
                    {
                        const auto file = fc.getResult();
                        if (safe != nullptr && file != juce::File{})
                            safe->processor.saveKitToFile (file.withFileExtension ("asamplerkit"), kitMode);
                    });
            });
    }

    void ASamplerEditor::loadKit()
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Load a kit", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.asamplerkit");

        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file != juce::File{})
                    processor.loadKitFromFile (file);
            });
    }

    void ASamplerEditor::chooseFactoryKit()
    {
        // JUCE's PopupMenu is keyboard navigable and exposed to UIA as a menu.
        juce::PopupMenu menu;
        const auto names = ASamplerProcessor::getFactoryKitNames();
        for (int i = 0; i < names.size(); ++i)
            menu.addItem (i + 1, names[i] == "Starter Drums"
                                     ? names[i] + " (replaces the samples on pads 1 to 8)"
                                     : names[i]);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (factoryKitButton.get()),
            [this] (int result)
            {
                if (result > 0)
                    processor.loadFactoryKit (result - 1);
            });
    }

    void ASamplerEditor::relink()
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

        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const auto dir = fc.getResult();
                if (dir != juce::File{})
                    processor.relinkMissingSamples (dir);
            });
    }

    void ASamplerEditor::clearAll()
    {
        // Destructive and wide-reaching, so confirm first. Without sight there
        // is no way to notice every pad emptying by accident.
        juce::NativeMessageBox::showOkCancelBox (
            juce::MessageBoxIconType::WarningIcon,
            "Clear all pads",
            "Reset all " + juce::String (kNumPads) + " pads to the default sample? You can undo this.",
            this,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                if (result == 1)
                    processor.clearAllPads();
                else
                    announce ("Clear all pads cancelled.");
            }));
    }

    void ASamplerEditor::exportMidi()
    {
        const int slices = processor.getCurrentChopCount();

        fileChooser = std::make_unique<juce::FileChooser> (
            "Export slice MIDI",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("ASampler-slices.mid"),
            "*.mid");

        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, slices] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file != juce::File{})
                    processor.exportSliceMidi (file.withFileExtension ("mid"), slices);
            });
    }

    juce::String ASamplerEditor::getHelpText()
    {
        return "ASampler " + ASamplerProcessor::getVersionString() + " keyboard help. "
               "Arrow keys move between pads; up is higher pad numbers. "
               "There are two banks of 16 pads: A is pads 1 to 16, B is pads 17 to 32. "
               "Page Up and Page Down switch bank. "
               "Space plays the selected pad. " + kCmd + " and a pad number, then letting go of " + kCmd
               + ", jumps to that pad. "
               "L loads samples; choose several to spread them over pads or layers. Audio files can also be "
               "dropped on a pad, or copied in Explorer and pasted with " + kCmd + " V. "
               "N and P load the next or previous sample in the same folder. "
               "Shift N and Shift P try the next or previous sample on the pad without replacing it: "
               "Enter keeps it, Escape goes back. "
               "T edits Start and O edits End by ear: Left and Right move 10 milliseconds, with " + kCmd
               + " 1, with Shift 100, Enter keeps, Escape restores. "
               "K learns the pad's note from the next MIDI note. F2 renames the pad. "
               "I describes the sample. B bounces the pad to the next empty pad. "
               "U tunes the pad to the sample's detected pitch. "
               + kCmd + " S saves a kit, " + kCmd + " O opens one. "
               + kCmd + " Z undoes, " + kCmd + " Y redoes. "
               "In REAPER, " + kCmd + " R toggles record and " + kCmd + " G grabs the selected item onto the pad. "
               "Z X C V, A S D F, Q W E R and 1 2 3 4 play the 16 pads of the current bank while held, and "
               "send the notes to the plugin's MIDI output; with Shift they say what the pad holds. "
               "After the pads, Tab reaches the selected pad's settings: choose a section from the list, "
               "then Tab through its knobs, lists and checkboxes. Arrows change a knob by 1 percent, "
               "with Shift 10, Home and End go to the ends, and Enter lets you type a value. "
               + kCmd + " B swaps the settings for the sample browser: arrows move through a folder and "
               "play each file, Enter loads it onto the selected pad or opens a folder, Backspace goes up, "
               "and a letter jumps to the next name starting with it. "
               "Shift F10 or the Applications key opens the pad menu with every action for the pad. "
               "Every setting is also a parameter in the host's parameter list; "
               "the Focus group there follows the selected pad.";
    }

    void ASamplerEditor::showHelp()
    {
        // Spoken even if it was the last thing said: asking again means the
        // user wants to hear it again.
        lastAnnounced.clear();
        announce (getHelpText());
    }

    // ---- Labels ---------------------------------------------------------------

    void ASamplerEditor::refreshLabels()
    {
        const int sel   = selectedPad();
        const int bank  = currentBank();
        const int layer = processor.getCurrentEditLayer();
        const auto padN = "Pad " + juce::String (sel + 1);
        const auto layerSuffix = layer > 0 ? ", layer " + juce::String (layer + 1) : juce::String();

        if (settingsPanel != nullptr)
            settingsPanel->setPad (sel);
        if (waveform != nullptr)
            waveform->setPad (sel);

        const auto gridText = "Pads, bank " + bankName (bank) + ", pads " + bankRange (bank)
                            + ". Arrow keys move, Page Up and Page Down switch bank, Space plays.";
        if (gridLabel.getText() != gridText)
        {
            gridLabel.setText (gridText, juce::dontSendNotification);
            gridLabel.setTitle (gridText);
        }
        const int otherBank = (bank + 1) % (kNumPads / kPadsPerBank);
        setButtonLabel (*bankButton, "Bank " + bankName (otherBank) + ", pads " + bankRange (otherBank)
                                     + " (Page " + juce::String (otherBank > bank ? "Up" : "Down") + ")");

        // A bank change is said once; focus landing on the new pad then
        // speaks the pad itself.
        if (bank != lastBank)
        {
            lastBank = bank;
            announce ("Bank " + bankName (bank) + ", pads " + bankRange (bank));
        }

        for (int i = 0; i < kNumPads; ++i)
        {
            auto& b = *padButtons[(std::size_t) i];

            const bool inBank = (i / kPadsPerBank) == bank;
            if (b.isVisible() != inBank)
                b.setVisible (inBank);

            // Visible text stays short; the spoken title carries the detail:
            // what is loaded, which note plays it, and anything that would
            // make it silent.
            const auto padName = processor.getPadName (i);
            const auto title   = padTitle (i);
            const auto shown   = padName.isNotEmpty() ? padName : processor.getPadSampleDisplayName (i);
            const auto text    = juce::String (i + 1) + "  " + shown;
            if (b.getButtonText() != text) b.setButtonText (text);
            if (b.getTitle() != title)     b.setTitle (title);

            PadButton::Face face;
            face.number  = i + 1;
            face.name    = shown == "default" ? juce::String ("built-in tone") : shown;
            face.key     = padKeyName (i);
            face.content = processor.getPadContent (i);
            face.muted   = processor.isPadMuted (i);
            face.soloed  = processor.isPadSoloed (i);
            b.setFace (face);
            b.setToggleState (i == sel, juce::dontSendNotification);

            // Keep the grid's single Tab stop on the selected pad, including
            // when the selection was changed from the host's parameter list.
            const bool shouldFocus = (i == sel);
            if (b.getWantsKeyboardFocus() != shouldFocus && ! b.hasKeyboardFocus (false))
                b.setWantsKeyboardFocus (shouldFocus);
        }

        padActionsLabel.setText ("Selected: " + padN + ", " + processor.getPadSampleDisplayName (sel),
                                 juce::dontSendNotification);

        const bool trying = processor.isPreviewing (sel);

        const int chopCount = processor.getCurrentChopCount();
        setButtonLabel (*loadButton,     "Load sample into " + padN + layerSuffix + " (L)");
        setButtonLabel (*prevButton,     "Previous sample (P)");
        setButtonLabel (*nextButton,     "Next sample (N)");
        setButtonLabel (*tryPrevButton,  "Try previous sample (Shift+P)");
        setButtonLabel (*tryNextButton,  "Try next sample (Shift+N)");
        setButtonLabel (*keepTryButton,  trying ? "Keep " + processor.getPreviewFile (sel).getFileNameWithoutExtension()
                                                      + " on " + padN + " (Enter)"
                                                : juce::String ("Keep tried sample (Enter)"));
        setButtonLabel (*playButton,     "Play " + padN + " (Space)");
        setButtonLabel (*editStartButton, mode == Mode::EditStart ? "Keep Start of " + padN + " (Enter)"
                                                                  : "Edit Start of " + padN + " by ear (T)");
        setButtonLabel (*editEndButton,   mode == Mode::EditEnd ? "Keep End of " + padN + " (Enter)"
                                                                : "Edit End of " + padN + " by ear (O)");
        setButtonLabel (*trimButton,     "Trim silence on " + padN);
        setButtonLabel (*describeButton, "Describe " + padN + " (I)");
        setButtonLabel (*renameButton,   "Rename " + padN + " (F2)");
        setButtonLabel (*noteLearnButton, "Learn note for " + padN + "... (K)");
        setButtonLabel (*applyButton,    "Apply " + padN + " settings to other pads...");
        setButtonLabel (*resetButton,    "Reset " + padN + layerSuffix);
        // Say what chop will overwrite, not just what it does.
        setButtonLabel (*chopButton,     "Chop " + padN + " into " + juce::String (chopCount)
                                         + ", replacing pads 1 to " + juce::String (chopCount));
        setButtonLabel (*copyButton,     "Copy " + padN);
        setButtonLabel (*pasteButton,    "Paste onto " + padN);
        setButtonLabel (*normaliseButton, "Normalise " + padN + (layer > 0 ? layerSuffix : ", layer 1"));
        setButtonLabel (*snapButton,     "Snap " + padN + " to transients");
        setButtonLabel (*spreadButton,   "Spread velocity layers on " + padN);
        setButtonLabel (*bounceButton,   "Bounce " + padN + " (B)");
        setButtonLabel (*grabButton,     "Grab REAPER item onto " + padN + " (" + kCmd + "+G)");
        setButtonLabel (*renderStretchButton,  "Render " + padN + " stretch");
        setButtonLabel (*renderFormantsButton, "Render " + padN + " formants");
        setButtonLabel (*tuneButton,     "Tune " + padN + " (U)");
        setButtonLabel (*moreButton,     "More actions for " + padN + "... (Shift+F10)");
        setButtonLabel (*browserButton,  browserShown ? "Show pad settings (" + kCmd + "+B)"
                                                      : "Show sample browser (" + kCmd + "+B)");

        setButtonLabel (*undoButton,       "Undo (" + kCmd + "+Z)");
        setButtonLabel (*redoButton,       "Redo (" + kCmd + "+Y)");
        setButtonLabel (*saveKitButton,    "Save kit... (" + kCmd + "+S)");
        setButtonLabel (*loadKitButton,    "Load kit... (" + kCmd + "+O)");
        setButtonLabel (*factoryKitButton, "Factory kit...");
        setButtonLabel (*clearAllButton,   "Clear all pads...");
        setButtonLabel (*exportMidiButton, "Export slice MIDI...");
        setButtonLabel (*helpButton,       "Keyboard help (F1)");

        // Unresolved samples show on the Relink button itself, so the problem
        // is found by tabbing, not only from a status message.
        const int missing = processor.getMissingSampleCount();
        setButtonLabel (*relinkButton, missing > 0
                                         ? "Relink " + juce::String (missing) + " missing sample"
                                               + (missing == 1 ? "..." : "s...")
                                         : juce::String ("Relink samples..."));
    }

    juce::String ASamplerEditor::padTitle (int i) const
    {
        juce::String title = "Pad " + juce::String (i + 1);
        if (const auto padName = processor.getPadName (i); padName.isNotEmpty())
            title << ", " << padName;
        if (processor.isPreviewing (i))
            title << ", trying " << processor.getPreviewFile (i).getFileName();
        else
            title << ", " << processor.getPadSampleDisplayName (i);
        title << ", " << processor.describePadTrigger (i);
        if (processor.isPadMuted (i))  title << ", muted";
        if (processor.isPadSoloed (i)) title << ", soloed";
        title << ", key " << padKeyName (i);
        return title;
    }

    void ASamplerEditor::refreshStatusLabel()
    {
        const auto msg = processor.getLastStatusMessage();
        if (msg.isNotEmpty())
            statusLabel.setText (msg, juce::dontSendNotification);
    }

    void ASamplerEditor::announce (const juce::String& message)
    {
        if (message.isEmpty())
            return;

        // Don't repeat what was just said. Bursty actions (a chop touches every
        // pad, a kit load every parameter) can fire the change listener many
        // times for one logical event.
        if (message == lastAnnounced)
            return;
        lastAnnounced = message;

        // Failures interrupt; routine confirmations wait their turn. High
        // priority everywhere talks over the user; low priority everywhere
        // lets an error scroll past unheard.
        const bool isProblem = message.containsIgnoreCase ("could not")
                            || message.containsIgnoreCase ("missing")
                            || message.containsIgnoreCase ("failed")
                            || message.containsIgnoreCase ("cannot")
                            || message.containsIgnoreCase ("not found")
                            || message.containsIgnoreCase ("unsupported");

        announcer.announce (message, isProblem);
    }

    // ---- Browser, pad menu, window size, moving parts ----------------------------

    void ASamplerEditor::showBrowser (bool shouldShow)
    {
        if (shouldShow && ! browserOpened)
        {
            // First time: the folder used last, else the selected pad's
            // sample folder, else the Music folder.
            browserOpened = true;
            juce::File start (uiSettings->get().getValue ("browserFolder"));
            juce::File select;
            if (! start.isDirectory())
            {
                const auto current = processor.getPadSampleFile (selectedPad());
                start  = current.getParentDirectory();
                select = current;
            }
            if (! start.isDirectory())
                start = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
            browser->setFolder (start, select);
        }

        if (! shouldShow && browserShown)
            processor.stopFilePreview();

        browserShown = shouldShow;
        browser->setVisible (shouldShow);
        settingsPanel->setVisible (! shouldShow);
        refreshLabels();

        lastAnnounced.clear();
        if (shouldShow)
        {
            browser->getList().grabKeyboardFocus();
            announce ("Sample browser, " + browser->getList().getTitle()
                      + ". Enter loads onto Pad " + juce::String (selectedPad() + 1) + ".");
        }
        else
        {
            settingsPanel->getSectionBox().grabKeyboardFocus();
            announce ("Pad settings");
        }
    }

    juce::Component* ASamplerEditor::menuTarget (juce::Component* preferred)
    {
        if (preferred != nullptr && preferred->isShowing())
            return preferred;
        if (auto* pad = padButtons[(std::size_t) selectedPad()].get(); pad != nullptr && pad->isShowing())
            return pad;
        return this;
    }

    namespace
    {
        constexpr int kSaveAudioItem = 1000;
    }

    juce::PopupMenu ASamplerEditor::buildPadMenu (int padIndex)
    {
        if (padIndex != selectedPad())
            selectPad (padIndex, false);
        refreshLabels();

        // Item id = 1 + index in this list; the labels are the buttons' own,
        // so the menu says exactly what each would do, shortcut included.
        auto add = [] (juce::PopupMenu& m, juce::TextButton* b, int id, bool enabled = true)
        {
            m.addItem (id, b->getButtonText(), enabled);
        };

        juce::PopupMenu menu;
        menu.addSectionHeader (padTitle (padIndex));

        const auto buttons = std::vector<juce::TextButton*> {
            loadButton.get(), prevButton.get(), nextButton.get(), tryPrevButton.get(), tryNextButton.get(),
            keepTryButton.get(), resetButton.get(),
            playButton.get(), editStartButton.get(), editEndButton.get(), trimButton.get(), snapButton.get(),
            describeButton.get(), renameButton.get(), noteLearnButton.get(),
            copyButton.get(), pasteButton.get(), applyButton.get(), normaliseButton.get(), spreadButton.get(),
            chopButton.get(), bounceButton.get(), renderStretchButton.get(), renderFormantsButton.get(),
            grabButton.get(),
            // appended, so the ids above stay put; shown in Play and edit
            tuneButton.get() };

        juce::PopupMenu sample, edit, pads, render;
        for (int i = 0; i < (int) buttons.size(); ++i)
        {
            auto* b = buttons[(std::size_t) i];
            const int id = i + 1;
            if (b == tuneButton.get()) add (edit, b, id);
            else if (i <= 6)  add (sample, b, id, b != keepTryButton.get() || processor.isPreviewing (padIndex));
            else if (i <= 14) add (edit, b, id);
            else if (i <= 19) add (pads, b, id);
            else              add (render, b, id, b != grabButton.get() || processor.getReaperApi().canGrabItems());
        }
        render.addItem (kSaveAudioItem, "Save " + juce::String ("Pad ") + juce::String (padIndex + 1)
                                          + " as audio file...");

        menu.addSubMenu ("Sample", sample);
        menu.addSubMenu ("Play and edit", edit);
        menu.addSubMenu ("Copy and apply", pads);
        menu.addSubMenu ("Chop, bounce and render", render);
        return menu;
    }

    void ASamplerEditor::runPadMenuItem (int itemId)
    {
        if (itemId == kSaveAudioItem)
        {
            savePadAudioAs();
            return;
        }

        const auto buttons = std::vector<juce::TextButton*> {
            loadButton.get(), prevButton.get(), nextButton.get(), tryPrevButton.get(), tryNextButton.get(),
            keepTryButton.get(), resetButton.get(),
            playButton.get(), editStartButton.get(), editEndButton.get(), trimButton.get(), snapButton.get(),
            describeButton.get(), renameButton.get(), noteLearnButton.get(),
            copyButton.get(), pasteButton.get(), applyButton.get(), normaliseButton.get(), spreadButton.get(),
            chopButton.get(), bounceButton.get(), renderStretchButton.get(), renderFormantsButton.get(),
            grabButton.get(),
            // appended, so the ids above stay put; shown in Play and edit
            tuneButton.get() };

        if (juce::isPositiveAndBelow (itemId - 1, (int) buttons.size()))
            if (auto* b = buttons[(std::size_t) (itemId - 1)]; b != nullptr && b->onClick)
                b->onClick();
    }

    void ASamplerEditor::showPadMenu (int padIndex)
    {
        auto menu = buildPadMenu (padIndex);
        auto* target = padButtons[(std::size_t) padIndex]->isShowing()
                         ? static_cast<juce::Component*> (padButtons[(std::size_t) padIndex].get())
                         : static_cast<juce::Component*> (this);

        juce::Component::SafePointer<ASamplerEditor> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
            [safe] (int result)
            {
                if (safe == nullptr)
                    return;
                // The key's release went to the menu, not to us.
                safe->appsKeyDown = false;
                if (result > 0)
                    safe->runPadMenuItem (result);
            });
    }

    void ASamplerEditor::savePadAudioAs()
    {
        const int pad = selectedPad();
        fileChooser = std::make_unique<juce::FileChooser> (
            "Save Pad " + juce::String (pad + 1) + " as audio",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                .getChildFile (processor.getPadAudioFileStem (pad) + ".wav"),
            "*.wav");

        juce::Component::SafePointer<ASamplerEditor> safe (this);
        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe, pad] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (safe != nullptr && file != juce::File{})
                    safe->processor.savePadAudio (pad, file.withFileExtension ("wav"));
            });
    }

    void ASamplerEditor::showSizeMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Window size");
        for (int i = 0; i < (int) kScales.size(); ++i)
        {
            const bool current = std::abs (kScales[(std::size_t) i] - uiScale) < 0.01f;
            menu.addItem (i + 1, juce::String (juce::roundToInt (kScales[(std::size_t) i] * 100.0f)) + "%",
                          true, current);
        }

        juce::Component::SafePointer<ASamplerEditor> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuTarget (sizeButton.get())),
            [safe] (int result)
            {
                if (safe == nullptr || result <= 0) return;
                safe->setUiScale (kScales[(std::size_t) (result - 1)], true);
                safe->lastAnnounced.clear();
                safe->announce ("Window size " + juce::String (juce::roundToInt (safe->uiScale * 100.0f)) + "%");
            });
    }

    bool ASamplerEditor::isInterestedInDragSource (const SourceDetails& details)
    {
        return details.description.toString().startsWith ("file:");
    }

    void ASamplerEditor::itemDropped (const SourceDetails& details)
    {
        const auto desc = details.description.toString();
        if (! desc.startsWith ("file:"))
            return;

        // Only onto a pad; a drop anywhere else is a drag that went nowhere.
        const auto where = content.getLocalPoint (this, details.localPosition.toInt());
        for (int i = 0; i < kNumPads; ++i)
        {
            auto& b = *padButtons[(std::size_t) i];
            if (b.isVisible() && b.getBounds().contains (where))
            {
                selectPad (i, false);
                loadFiles (i, { juce::File (desc.fromFirstOccurrenceOf ("file:", false, false)) });
                return;
            }
        }
    }

    bool ASamplerEditor::shouldDropFilesWhenDraggedExternally (const SourceDetails& details,
                                                                juce::StringArray& files, bool& canMoveFiles)
    {
        canMoveFiles = false;
        const auto desc = details.description.toString();

        if (desc.startsWith ("file:"))
        {
            files.add (desc.fromFirstOccurrenceOf ("file:", false, false));
            return true;
        }

        if (desc.startsWith ("pad:"))
        {
            // Rendered now, as a bounce would be, into a folder kept for the
            // purpose: the host copies or references the file once dropped.
            const int pad = desc.fromFirstOccurrenceOf ("pad:", false, false).getIntValue();
            const auto file = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                  .getChildFile ("ASampler").getChildFile ("exports")
                                  .getChildFile (processor.getPadAudioFileStem (pad) + ".wav");
            if (! processor.savePadAudio (pad, file, false))
                return false;
            files.add (file.getFullPathName());
            return true;
        }
        return false;
    }

    void ASamplerEditor::frame()
    {
        if (waveform != nullptr && waveform->isShowing())
            waveform->tick();

        meter.setLevel (processor.takeOutputPeak());

        const int bank = currentBank();
        for (int i = bank * kPadsPerBank; i < (bank + 1) * kPadsPerBank && i < kNumPads; ++i)
        {
            auto& pad = *padButtons[(std::size_t) i];
            const auto count = processor.getPadHitCount (i);
            float level = pad.getFlash();
            if (count != lastHitCount[(std::size_t) i])
            {
                lastHitCount[(std::size_t) i] = count;
                level = 0.35f + 0.65f * processor.getPadHitVelocity (i);
            }
            else
            {
                level *= 0.88f;
                if (level < 0.02f) level = 0.0f;
            }
            pad.setFlash (level);
        }
    }

    float ASamplerEditor::getPadFlash (int padIndex) const
    {
        return juce::isPositiveAndBelow (padIndex, kNumPads) ? padButtons[(std::size_t) padIndex]->getFlash() : 0.0f;
    }

    // ---- Test hooks -----------------------------------------------------------

    juce::Button* ASamplerEditor::getPadButton (int padIndex) const
    {
        return juce::isPositiveAndBelow (padIndex, kNumPads) ? padButtons[(std::size_t) padIndex].get()
                                                             : nullptr;
    }

    juce::Button* ASamplerEditor::findButtonByTitlePrefix (const juce::String& prefix) const
    {
        for (auto* child : content.getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (child))
                if (b->getTitle().startsWith (prefix))
                    return b;
        return nullptr;
    }
}
