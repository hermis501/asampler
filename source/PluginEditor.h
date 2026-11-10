#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ScreenReaderAnnouncer.h"

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
    //   3. Pad actions, all applying to the selected pad and naming it in
    //      their labels, so each button says what it will touch.
    //   4. Kit actions.
    //   5. Status line.
    //
    // Shortcuts (anywhere in the editor): L load (several files at once
    // offers pads or layers), N / P next / previous sample in the folder,
    // Shift+N / Shift+P try the next / previous sample without replacing the
    // pad's (Enter keeps it, Escape goes back), T / O edit Start / End by ear
    // (arrows step 10 ms, Ctrl 1 ms, Shift 100 ms; Enter keeps, Escape
    // restores), K learn the pad's note from MIDI, I describe, B bounce, F2
    // rename, Ctrl+S save kit, Ctrl+O open kit, Ctrl+V paste files from
    // Explorer, Ctrl + digits jump to a pad (on releasing Ctrl), Ctrl+R / Ctrl+G
    // REAPER record / grab item, Ctrl+Z / Ctrl+Y undo / redo, F1 help.
    // Z X C V / A S D F / Q W E R / 1 2 3 4 play the 16 pads of the current
    // bank for as long as they are held, and send the notes out as MIDI;
    // with Shift they speak the pad instead. Audio files can also be dropped
    // onto a pad.
    //
    // Every control is a JUCE TextButton or Label, which expose UIA roles
    // without extra work. Results are spoken through ScreenReaderAnnouncer
    // (the screen reader's own voice), deduplicated and prioritised.
    class ASamplerEditor : public juce::AudioProcessorEditor,
                           public juce::ChangeListener,
                           public juce::FileDragAndDropTarget,
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

            bool keyPressed (const juce::KeyPress& key) override;
            void focusGained (FocusChangeType cause) override;
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
        void renderStretchSelected();
        void renderFormantsSelected();

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
                                          grabButton, renderStretchButton, renderFormantsButton;

        juce::Label kitLabel;
        std::unique_ptr<juce::TextButton> undoButton, redoButton, saveKitButton, loadKitButton,
                                          factoryKitButton, relinkButton, clearAllButton,
                                          exportMidiButton, helpButton;

        juce::Label statusLabel;

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

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ASamplerEditor)
    };
}
