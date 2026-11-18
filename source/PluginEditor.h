#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ParameterPanel.h"
#include "PluginProcessor.h"
#include "SampleBrowser.h"
#include "ScreenReaderAnnouncer.h"
#include "UiSettings.h"
#include "WaveformView.h"

#include <array>
#include <functional>
#include <map>
#include <memory>

namespace asampler
{
    // Accessible editor, keyboard first.
    //
    // Structure (also the Tab order):
    //   1. Heading, naming the plugin and its version.
    //   2. The bank button, then the pad grid: one bank of 16 pads laid out
    //      like an MPC (Pad 1 bottom left), bank A = pads 1-16, bank B =
    //      17-32. The grid is one Tab stop — only the selected pad takes focus
    //      — and the arrow keys move between pads, the standard pattern for a
    //      grid; up from the top row of bank A carries on into bank B, and
    //      Page Up / Page Down jump between banks. Moving onto a pad selects it
    //      (the "Focused Pad" parameter, so the Focus parameter group in the
    //      host follows along); Space or Enter plays it.
    //   3. The selected pad's settings (ParameterPanel): one section at a
    //      time, as knobs, lists and checkboxes bound to its parameters, so
    //      hosts without a usable parameter list (Studio One, Live, Logic)
    //      reach every setting too.
    //      Ctrl+B swaps it for the sample browser (SampleBrowser) in the
    //      same place; the button after it does the same.
    //   4. Pad actions, all applying to the selected pad and naming it in
    //      their labels, so each button says what it will touch. The common
    //      ones are buttons; every action, those included, is in the pad's
    //      menu ("More actions", Shift+F10, the Applications key, or a
    //      right-click on a pad).
    //   5. Kit actions, and the window size.
    //   6. Status line.
    //
    // The settings panel sits to the right of the grid, under the waveform
    // (WaveformView, mouse only, outside the Tab order and the accessibility
    // tree), so the Tab order is set explicitly rather than left to screen
    // position.
    //
    // Everything is laid out once at a fixed design size inside `content`,
    // which is scaled to the window: the window can be resized (keeping its
    // shape) or set from the Window size menu, and the choice is remembered
    // on this computer (UiSettings).
    //
    // Shortcuts (anywhere in the editor): L load (several files at once
    // offers pads or layers), N / P next / previous sample in the folder,
    // Shift+N / Shift+P try the next / previous sample without replacing the
    // pad's (Enter keeps it, Escape goes back), T / O edit Start / End by ear
    // (arrows step 10 ms, Ctrl 1 ms, Shift 100 ms; Enter keeps, Escape
    // restores), K learn the pad's note from MIDI, I describe, B bounce, F2
    // rename, Ctrl+S save kit, Ctrl+O open kit, Ctrl+V paste files from
    // Explorer, Ctrl + digits jump to a pad (on releasing Ctrl), Ctrl+R / Ctrl+G
    // REAPER record / grab item, Ctrl+Z / Ctrl+Y undo / redo, Ctrl+B sample
    // browser, Shift+F10 or the Applications key the pad menu, F1 help.
    // Z X C V / A S D F / Q W E R / 1 2 3 4 play the 16 pads of the current
    // bank for as long as they are held, and send the notes out as MIDI;
    // with Shift they speak the pad instead. Audio files can also be dropped
    // onto a pad.
    //
    // Every control is a JUCE TextButton, Label, Slider, ComboBox or
    // ToggleButton, which expose UIA roles without extra work. Results are spoken through ScreenReaderAnnouncer
    // (the screen reader's own voice), deduplicated and prioritised.
    class ASamplerEditor : public juce::AudioProcessorEditor,
                           public juce::ChangeListener,
                           public juce::FileDragAndDropTarget,
                           public juce::DragAndDropContainer,
                           public juce::DragAndDropTarget,
                           private juce::Timer
    {
    public:
        explicit ASamplerEditor (ASamplerProcessor& proc);
        ~ASamplerEditor() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void visibilityChanged() override;

        void changeListenerCallback (juce::ChangeBroadcaster*) override;
        bool keyPressed (const juce::KeyPress& key) override;
        bool keyStateChanged (bool isKeyDown) override;
        void modifierKeysChanged (const juce::ModifierKeys& mods) override;

        bool isInterestedInFileDrag (const juce::StringArray& files) override;
        void filesDropped (const juce::StringArray& files, int x, int y) override;

        // Rows dragged from the sample browser onto a pad.
        bool isInterestedInDragSource (const SourceDetails&) override;
        void itemDropped (const SourceDetails&) override;
        // A pad or browser row dragged out of the window becomes a file: the
        // pad rendered as a bounce would be, or the browser's file itself.
        bool shouldDropFilesWhenDraggedExternally (const SourceDetails&, juce::StringArray& files,
                                                   bool& canMoveFiles) override;

        // The size everything is laid out at; the window shows it scaled.
        static constexpr int kDesignWidth  = 1340;
        static constexpr int kDesignHeight = 704;
        // Window scale, 1 = the design size. `persist` stores it for the next
        // window on this computer.
        void  setUiScale (float scale, bool persist);
        float getUiScale() const noexcept { return uiScale; }

        // What the arrow and Enter / Escape keys currently do.
        enum class Mode { Normal, EditStart, EditEnd, TrySample };
        Mode getMode() const noexcept { return mode; }

        // ---- For tests ----
        juce::Button* getPadButton (int padIndex) const;
        juce::Button* findButtonByTitlePrefix (const juce::String& prefix) const;
        const juce::String& getLastAnnouncement() const noexcept { return lastAnnounced; }
        void refreshLabelsNow() { refreshLabels(); }
        // Load dropped / pasted files onto `padIndex`: one file loads straight
        // away, several open the pads-or-layers menu (or use `forcedMode`).
        void loadFiles (int padIndex, const juce::Array<juce::File>& files, int forcedMode = -1);
        ParameterPanel& getSettingsPanel() noexcept { return *settingsPanel; }
        SampleBrowser&  getBrowser() noexcept { return *browser; }
        WaveformView&   getWaveform() noexcept { return *waveform; }
        bool isBrowserShown() const noexcept { return browserShown; }
        void showBrowser (bool shouldShow);
        // The pad menu for `padIndex`, as it would appear.
        juce::PopupMenu buildPadMenu (int padIndex);
        // Runs the pad-menu item with this id (as buildPadMenu numbers them).
        void runPadMenuItem (int itemId);
        // Draws one frame of the moving parts (pad flashes, meter, playhead).
        void frame();
        float getPadFlash (int padIndex) const;

        // The text read by F1 / the Keyboard help button.
        static juce::String getHelpText();

    private:
        // A pad: a TextButton that also hands arrow keys to the grid logic and
        // selects its pad when it receives focus.
        class PadButton : public juce::TextButton
        {
        public:
            std::function<bool (const juce::KeyPress&)> onKeyFirst;   // edit modes, before anything else
            std::function<bool (const juce::KeyPress&)> onArrowKey;
            std::function<void()>                       onFocus;
            std::function<void()>                       onContextMenu;   // right-click
            std::function<void()>                       onDragOut;       // dragged far enough to leave

            // What the pad face shows. The spoken title is set separately.
            struct Face
            {
                int          number = 1;
                juce::String name, key;
                ASamplerProcessor::PadContent content = ASamplerProcessor::PadContent::BuiltIn;
                bool muted = false, soloed = false;
                bool operator== (const Face& o) const
                {
                    return number == o.number && name == o.name && key == o.key && content == o.content
                        && muted == o.muted && soloed == o.soloed;
                }
            };
            void setFace (const Face& f)  { if (! (f == face)) { face = f; repaint(); } }
            void setFlash (float level)   { if (std::abs (level - flash) > 0.01f || (level == 0.0f && flash != 0.0f)) { flash = level; repaint(); } }
            float getFlash() const noexcept { return flash; }

            bool keyPressed (const juce::KeyPress& key) override;
            void focusGained (FocusChangeType cause) override;
            void focusLost (FocusChangeType cause) override;
            void mouseDown (const juce::MouseEvent&) override;
            void mouseDrag (const juce::MouseEvent&) override;
            void mouseUp (const juce::MouseEvent&) override;
            void paintButton (juce::Graphics&, bool isOver, bool isDown) override;

        private:
            Face  face;
            float flash = 0.0f;
            bool  popupClick = false, draggedOut = false;
        };

        // Main output level, drawn as a bar. Sight only.
        class OutputMeter : public juce::Component
        {
        public:
            OutputMeter() { setAccessible (false); setInterceptsMouseClicks (false, false); }
            void setLevel (float linear);
            void paint (juce::Graphics&) override;
        private:
            float level = 0.0f, shown = -1.0f;
        };

        void timerCallback() override;

        void selectPad (int padIndex, bool moveFocus);
        bool handleGridArrow (int padIndex, const juce::KeyPress& key);

        // Pad actions (selected pad).
        void loadSample();
        void browse (int direction);
        void playSelected();
        void describeSelected();
        void resetSelected();
        void chopSelected();
        void copySelected();
        void pasteSelected();
        void normaliseSelected();
        void snapSelected();
        void bounceSelected();
        void tuneSelected();
        void findSetting();
        void toggleAutomation();
        void renderStretchSelected();
        void renderFormantsSelected();
        void savePadAudioAs();
        void showPadMenu (int padIndex);
        void showSizeMenu();
        void layoutContent();
        // Where a popup menu for a hidden button should appear.
        juce::Component* menuTarget (juce::Component* preferred);

        void editSlicePoint (bool isEnd);
        void trySample (int direction);
        void keepTriedSample();
        void renameSelected();
        void showApplyMenu();
        void showNoteLearnMenu();
        void trimSelected();
        void spreadSelected();
        void grabReaperItem();
        void pasteFiles();
        void speakPad (int padIndex);

        // Keys that belong to the current edit mode (arrows, Enter, Escape,
        // Space while editing a slice point; Enter / Escape while trying a
        // sample). True if consumed.
        bool handleModeKey (const juce::KeyPress& key);
        void leaveMode();

        // Kit actions.
        void saveKit();
        void loadKit();
        void chooseFactoryKit();
        void relink();
        void clearAll();
        void exportMidi();
        void showHelp();

        void refreshLabels();
        void refreshStatusLabel();
        void announce (const juce::String& message);

        // The pad a QWERTY key plays in `bank`, or -1. padForKey rejects
        // modifiers; padForKeyCode looks at the key alone.
        static int padForKey (const juce::KeyPress& key, int bank);
        static int padForKeyCode (int keyCode, int bank);
        juce::String padTitle (int padIndex) const;

        juce::TextButton& addButton (std::unique_ptr<juce::TextButton>& slot,
                                     std::function<void()> onClick,
                                     const juce::String& description = {});

        int selectedPad() const { return processor.getCurrentFocusedPad(); }
        int currentBank() const { return selectedPad() / kPadsPerBank; }
        void switchBank();

        ASamplerProcessor& processor;

        juce::LookAndFeel_V4 lookAndFeel;
        juce::SharedResourcePointer<UiSettings> uiSettings;

        // Every visible control lives in here, laid out at the design size
        // and scaled to the window.
        juce::Component content;
        float uiScale      = 1.0f;
        bool  persistScale = false;

        juce::Label heading;
        juce::Label gridLabel;
        std::unique_ptr<juce::TextButton> bankButton;
        std::array<std::unique_ptr<PadButton>, kNumPads> padButtons;
        int lastBank = 0;   // to announce a bank change once

        juce::Label padActionsLabel;
        std::unique_ptr<juce::TextButton> loadButton, prevButton, nextButton, tryPrevButton, tryNextButton,
                                          keepTryButton, playButton, editStartButton, editEndButton,
                                          trimButton, describeButton, renameButton, noteLearnButton,
                                          applyButton, resetButton, chopButton, copyButton,
                                          pasteButton, normaliseButton, spreadButton, snapButton, bounceButton,
                                          grabButton, renderStretchButton, renderFormantsButton,
                                          tuneButton, moreButton;

        juce::Label kitLabel;
        std::unique_ptr<juce::TextButton> undoButton, redoButton, saveKitButton, loadKitButton,
                                          factoryKitButton, relinkButton, clearAllButton,
                                          exportMidiButton, helpButton, sizeButton;

        juce::Label statusLabel;

        std::unique_ptr<ParameterPanel> settingsPanel;
        std::unique_ptr<SampleBrowser>  browser;
        std::unique_ptr<WaveformView>   waveform;
        std::unique_ptr<juce::TextButton> browserButton;   // swaps settings and browser
        OutputMeter meter;
        bool browserShown = false, browserOpened = false;

        // Pad flashes: the hit count last seen, per pad.
        std::array<uint32_t, kNumPads> lastHitCount {};
        bool appsKeyDown = false;

        Mode  mode = Mode::Normal;
        int   modePad = 0;          // the pad the mode belongs to
        float scrubValue = 0.0f;    // Start / End being edited, 0..1

        // Pad keys being held: key code -> MIDI note sent, so release sends
        // the matching note-off even if the pad's note changed meanwhile.
        std::map<int, int> heldPadKeys;

        // Digits typed with Ctrl held; the pad is selected when Ctrl is let go.
        juce::String jumpDigits;

        // Last message spoken, so an unchanged status isn't announced twice.
        // Repeated identical speech is noise the user has to sit through.
        juce::String lastAnnounced;

        // Routes announcements to the screen reader's own voice (UIA
        // notifications on Windows) instead of JUCE's separate SAPI voice.
        ScreenReaderAnnouncer announcer { *this };

        std::unique_ptr<juce::FileChooser> fileChooser;  // must outlive launchAsync callback

        // Shows button and pad tooltips on mouse hover.
        juce::TooltipWindow tooltipWindow { this, 700 };

        // Last, so it stops before anything it draws is destroyed.
        juce::VBlankAttachment vblank { this, [this] { frame(); } };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ASamplerEditor)
    };
}
