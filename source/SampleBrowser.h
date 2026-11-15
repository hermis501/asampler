#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

#include <functional>

namespace asampler
{
    // A folder of samples inside the window: folders first, then audio files.
    //
    // It is a plain JUCE ListBox, so a screen reader reads each row ("Folder
    // Kicks", "Kick 01.wav, 3 of 40") and the arrows move through it like any
    // list. Moving onto a file plays it (when "Preview" is ticked) without
    // touching any pad; Enter or a double-click loads it onto the selected
    // pad; Enter on a folder opens it and Backspace goes up. A letter jumps
    // to the next row starting with it. Rows can also be dragged onto a pad,
    // or out of the window into the host.
    //
    // The OS file chooser (L) stays the main way to load; this is for
    // auditioning a folder quickly, the way sighted users expect a sampler to
    // let them.
    class SampleBrowser : public juce::Component,
                          private juce::ListBoxModel
    {
    public:
        explicit SampleBrowser (ASamplerProcessor& proc);
        ~SampleBrowser() override;

        // Load `file` onto the selected pad (the editor decides how).
        std::function<void (const juce::File&)> onLoad;
        // Offered every key first, like the settings panel.
        std::function<bool (const juce::KeyPress&)> onKeyFirst;
        // Called when the folder changes, to remember it.
        std::function<void (const juce::File&)> onFolderChanged;

        // An empty File lists the drives.
        void setFolder (const juce::File& folder, const juce::File& select = {});
        juce::File getFolder() const noexcept { return folder; }
        void goUp();

        // ---- For tests ----
        int  getNumEntries() const noexcept { return entries.size(); }
        juce::File getEntry (int row) const { return entries[row]; }
        juce::String getRowName (int row) const;
        juce::ListBox& getList() noexcept { return list; }
        void activateRow (int row);
        bool isPreviewOn() const noexcept { return previewToggle.getToggleState(); }

        void resized() override;
        void paint (juce::Graphics&) override;

    private:
        class List;

        // ListBoxModel
        int  getNumRows() override { return entries.size(); }
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        juce::String getNameForRow (int row) override { return getRowName (row); }
        void selectedRowsChanged (int lastRowSelected) override;
        void returnKeyPressed (int lastRowSelected) override { activateRow (lastRowSelected); }
        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { activateRow (row); }
        juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;

        void refreshFolderLabel();
        bool jumpToLetter (juce::juce_wchar c);

        ASamplerProcessor& processor;

        juce::Label        folderLabel;
        juce::TextButton   upButton, chooseButton;
        juce::ToggleButton previewToggle { "Preview" };
        std::unique_ptr<List> listOwner;
        juce::ListBox&     list;

        juce::File              folder;
        juce::Array<juce::File> entries;   // folders first, then files
        int                     numFolders = 0;
        bool                    suppressPreview = false;

        std::unique_ptr<juce::FileChooser> chooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleBrowser)
    };
}
