#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

#include <array>
#include <memory>

namespace asampler
{
    // Accessible custom editor.
    //
    // Design notes (screen-reader first):
    //   - Per-pad "Load" and "Reset" TextButtons. JUCE's TextButton already
    //     exposes a Button accessibility role to Windows UIA, so NVDA/JAWS
    //     announce them as "<label>, button" with no extra plumbing.
    //   - Linear Tab order: Load 1, Reset 1, Load 2, Reset 2, ..., status.
    //     Predictable for keyboard navigation, no nested focus regions.
    //   - Each load button's text encodes the pad's current state — screen
    //     reader speaks the current filename when focus lands on the button.
    //   - After a load succeeds we post a UIA Notification announcement so the
    //     screen reader speaks the result even if focus didn't change.
    //   - Sample selection uses juce::FileChooser — wraps the native OS dialog,
    //     which is already accessible on Windows. No custom file browser.
    class ASamplerEditor : public juce::AudioProcessorEditor,
                           public juce::ChangeListener
    {
    public:
        explicit ASamplerEditor (ASamplerProcessor& proc);
        ~ASamplerEditor() override;

        void paint (juce::Graphics&) override;
        void resized() override;

        void changeListenerCallback (juce::ChangeBroadcaster*) override;

        // QWERTY pad triggering, so a sample can be auditioned without any
        // external MIDI hardware. Laid out like an MPC: Z-row is pads 1-4,
        // then A, Q and number rows going up.
        bool keyPressed (const juce::KeyPress& key) override;

    private:
        void onLoadClicked     (int padIndex);
        void onAuditionClicked (int padIndex);
        void onResetClicked    (int padIndex);
        void onChopClicked     (int padIndex);
        void onVocalSpeakClicked();
        void onVocalChopClicked();
        void onVocalSpreadClicked();

        // Kit / utility actions.
        void onUndoClicked();
        void onRedoClicked();
        void onSaveKitClicked();
        void onLoadKitClicked();
        void onFactoryKitClicked();
        void onRelinkClicked();
        void onCopyPadClicked();
        void onPastePadClicked();
        void onClearAllClicked();
        void onExportMidiClicked();
        void onNormaliseClicked();
        void onSnapTransientsClicked();
        void onRenderStretchClicked();
        void onRenderFormantsClicked();

        void refreshVocalLabels();
        void refreshUtilityLabels();
        void refreshPadButtonLabels();
        void refreshStatusLabel();
        void announce (const juce::String& message);

        // Map a key press onto a 0-based pad index, or -1 if it isn't a pad key.
        static int padForKey (const juce::KeyPress& key);

        // Configure a button for screen-reader use in one place: focusable,
        // visible text and accessible title kept identical, click handler set.
        juce::TextButton& addButton (std::unique_ptr<juce::TextButton>& slot,
                                     const juce::String& text,
                                     std::function<void()> onClick,
                                     const juce::String& description = {});

        ASamplerProcessor& processor;

        juce::Label                                                  heading;

        // Vocal (SAPI5 text-to-speech) generation row: type text, then generate
        // it onto the target pad as a choppable / spreadable vocal sample.
        juce::Label      vocalLabel;
        juce::TextEditor vocalEditor;
        juce::TextButton vocalSpeakButton;
        juce::TextButton vocalChopButton;
        juce::TextButton vocalSpreadButton;
        // Kit and pad-utility row. These are the actions that make the plugin
        // survivable in real use: undo for destructive chops, kits so a set-up
        // drum rack outlives one project, relink for moved samples.
        juce::Label                      utilityLabel;
        std::unique_ptr<juce::TextButton> undoButton;
        std::unique_ptr<juce::TextButton> redoButton;
        std::unique_ptr<juce::TextButton> saveKitButton;
        std::unique_ptr<juce::TextButton> loadKitButton;
        std::unique_ptr<juce::TextButton> factoryKitButton;
        std::unique_ptr<juce::TextButton> relinkButton;
        std::unique_ptr<juce::TextButton> copyPadButton;
        std::unique_ptr<juce::TextButton> pastePadButton;
        std::unique_ptr<juce::TextButton> clearAllButton;
        std::unique_ptr<juce::TextButton> exportMidiButton;
        std::unique_ptr<juce::TextButton> normaliseButton;
        std::unique_ptr<juce::TextButton> snapTransientsButton;
        std::unique_ptr<juce::TextButton> renderStretchButton;
        std::unique_ptr<juce::TextButton> renderFormantsButton;

        std::array<std::unique_ptr<juce::TextButton>, kNumPads>      loadButtons;
        std::array<std::unique_ptr<juce::TextButton>, kNumPads>      auditionButtons;
        std::array<std::unique_ptr<juce::TextButton>, kNumPads>      resetButtons;
        std::array<std::unique_ptr<juce::TextButton>, kNumPads>      chopButtons;
        juce::Label                                                  statusLabel;

        // Last message spoken, so an unchanged status isn't announced twice.
        // Repeated identical speech is noise the user has to sit through.
        juce::String                                                 lastAnnounced;

        std::unique_ptr<juce::FileChooser> fileChooser;  // must outlive launchAsync callback

        // Live link to the host's "Chop Count" parameter so the chop buttons'
        // labels and screen-reader titles update when the value changes
        // (whether the user did it from REAPER's FX list or from automation).
        std::unique_ptr<juce::ParameterAttachment> chopCountAttachment;

        // Same idea for "Edit Layer" — when the value moves we refresh the
        // Load button labels so the screen reader announces the file currently
        // sitting on the selected layer.
        std::unique_ptr<juce::ParameterAttachment> editLayerAttachment;

        // Keeps the vocal buttons' labels naming the current target pad, whether
        // it was changed here or from REAPER's FX parameter list.
        std::unique_ptr<juce::ParameterAttachment> vocalTargetAttachment;

        // Keeps the Copy/Paste/Normalise/Chop labels naming the focused pad,
        // so those buttons always say which pad they will act on.
        std::unique_ptr<juce::ParameterAttachment> focusedPadAttachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ASamplerEditor)
    };
}
