#include "SampleBrowser.h"

namespace asampler
{
    namespace
    {
        bool isAudio (const juce::File& f)
        {
            return f.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
        }

        juce::String folderTitle (const juce::File& folder)
        {
            return folder == juce::File() ? juce::String ("Drives") : folder.getFullPathName();
        }
    }

    // The list itself, with Backspace (up) and letter jumps on top of what
    // ListBox already does with the arrows, Home, End and Page keys.
    class SampleBrowser::List : public juce::ListBox
    {
    public:
        explicit List (SampleBrowser& o) : juce::ListBox ("Samples", &o), owner (o) {}

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (owner.onKeyFirst && owner.onKeyFirst (key))
                return true;

            if (key == juce::KeyPress::backspaceKey)
            {
                owner.goUp();
                return true;
            }

            if (key == juce::KeyPress::spaceKey)
            {
                const int row = getSelectedRow();
                if (juce::isPositiveAndBelow (row, owner.entries.size()) && owner.entries[row].existsAsFile())
                    owner.processor.previewFile (owner.entries[row]);
                return true;
            }

            const auto mods = key.getModifiers();
            const auto c = key.getTextCharacter();
            if (! mods.isCommandDown() && ! mods.isAltDown() && ! mods.isCtrlDown()
                && juce::CharacterFunctions::isLetterOrDigit (c))
                return owner.jumpToLetter (c);

            return juce::ListBox::keyPressed (key);
        }

    private:
        SampleBrowser& owner;
    };

    SampleBrowser::SampleBrowser (ASamplerProcessor& proc)
        : processor (proc),
          listOwner (std::make_unique<List> (*this)),
          list (*listOwner)
    {
        folderLabel.setJustificationType (juce::Justification::centredLeft);
        folderLabel.setMinimumHorizontalScale (0.6f);
        folderLabel.setAccessible (false);   // the list's title carries the folder
        addAndMakeVisible (folderLabel);

        upButton.setButtonText ("Up (Backspace)");
        upButton.setTitle ("Up one folder (Backspace)");
        upButton.onClick = [this] { goUp(); };
        addAndMakeVisible (upButton);

        chooseButton.setButtonText ("Choose folder...");
        chooseButton.setTitle ("Choose folder...");
        chooseButton.setTooltip ("Pick the folder to browse.");
        chooseButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Folder to browse",
                folder.isDirectory() ? folder : juce::File::getSpecialLocation (juce::File::userMusicDirectory));
            juce::Component::SafePointer<SampleBrowser> safe (this);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                [safe] (const juce::FileChooser& fc)
                {
                    if (safe == nullptr) return;
                    const auto dir = fc.getResult();
                    if (dir.isDirectory())
                    {
                        safe->setFolder (dir);
                        safe->list.grabKeyboardFocus();
                    }
                });
        };
        addAndMakeVisible (chooseButton);

        previewToggle.setTitle ("Preview on select");
        previewToggle.setToggleState (true, juce::dontSendNotification);
        previewToggle.onClick = [this] { if (! previewToggle.getToggleState()) processor.stopFilePreview(); };
        addAndMakeVisible (previewToggle);

        list.setRowHeight (22);
        list.setMultipleSelectionEnabled (false);
        list.setWantsKeyboardFocus (true);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff15191e));
        addAndMakeVisible (list);

        // Tab order inside the browser: the list first, it is what matters.
        list.setExplicitFocusOrder (1);
        upButton.setExplicitFocusOrder (2);
        chooseButton.setExplicitFocusOrder (3);
        previewToggle.setExplicitFocusOrder (4);
        setFocusContainerType (juce::Component::FocusContainerType::none);
    }

    SampleBrowser::~SampleBrowser()
    {
        list.setModel (nullptr);
    }

    void SampleBrowser::setFolder (const juce::File& newFolder, const juce::File& select)
    {
        folder = newFolder.isDirectory() ? newFolder : juce::File();
        entries.clear();
        numFolders = 0;

        if (folder == juce::File())
        {
            juce::File::findFileSystemRoots (entries);
            numFolders = entries.size();
        }
        else
        {
            auto dirs  = folder.findChildFiles (juce::File::findDirectories, false);
            auto files = folder.findChildFiles (juce::File::findFiles, false);

            auto byName = [] (const juce::File& a, const juce::File& b)
            {
                return a.getFileName().compareNatural (b.getFileName()) < 0;
            };
            std::sort (dirs.begin(), dirs.end(), byName);
            std::sort (files.begin(), files.end(), byName);

            for (const auto& d : dirs)
                if (! d.isHidden() && ! d.getFileName().startsWithChar ('.'))
                    entries.add (d);
            numFolders = entries.size();
            for (const auto& f : files)
                if (isAudio (f) && ! f.isHidden())
                    entries.add (f);
        }

        refreshFolderLabel();

        // Selecting the row below must not play anything: arriving in a folder
        // is not the same as choosing a sample.
        suppressPreview = true;
        list.updateContent();
        const int row = select != juce::File() ? entries.indexOf (select) : -1;
        list.selectRow (juce::jmax (0, row));
        list.scrollToEnsureRowIsOnscreen (juce::jmax (0, row));
        suppressPreview = false;
        list.repaint();

        if (onFolderChanged)
            onFolderChanged (folder);
    }

    void SampleBrowser::goUp()
    {
        if (folder == juce::File())
            return;
        const auto parent = folder.getParentDirectory();
        // At a drive's root the parent is itself: go to the list of drives.
        setFolder (parent == folder ? juce::File() : parent, folder);
    }

    void SampleBrowser::refreshFolderLabel()
    {
        const auto title = folderTitle (folder);
        folderLabel.setText (title, juce::dontSendNotification);
        const auto files = entries.size() - numFolders;
        list.setTitle ("Samples in " + (folder == juce::File() ? juce::String ("Drives") : folder.getFileName())
                       + ", " + juce::String (numFolders) + (numFolders == 1 ? " folder, " : " folders, ")
                       + juce::String (files) + (files == 1 ? " file" : " files"));
    }

    juce::String SampleBrowser::getRowName (int row) const
    {
        if (! juce::isPositiveAndBelow (row, entries.size()))
            return {};
        const auto& f = entries[row];
        if (row < numFolders)
            return (folder == juce::File() ? "Drive " + f.getFullPathName() : "Folder " + f.getFileName());
        const int files = entries.size() - numFolders;
        return f.getFileName() + ", " + juce::String (row - numFolders + 1) + " of " + juce::String (files);
    }

    void SampleBrowser::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
    {
        if (! juce::isPositiveAndBelow (row, entries.size()))
            return;
        if (selected)
            g.fillAll (juce::Colour (0xff2f5f8a));

        const bool isFolder = row < numFolders;
        g.setColour (isFolder ? juce::Colour (0xffe8c45f) : juce::Colours::white);
        g.setFont (juce::FontOptions (14.0f));
        const auto& f = entries[row];
        const auto text = isFolder ? (folder == juce::File() ? f.getFullPathName() : f.getFileName() + "/")
                                   : f.getFileName();
        g.drawText (text, 8, 0, width - 12, height, juce::Justification::centredLeft, true);
    }

    void SampleBrowser::selectedRowsChanged (int row)
    {
        if (suppressPreview || ! previewToggle.getToggleState())
            return;
        if (juce::isPositiveAndBelow (row, entries.size()) && row >= numFolders)
            processor.previewFile (entries[row]);
    }

    void SampleBrowser::activateRow (int row)
    {
        if (! juce::isPositiveAndBelow (row, entries.size()))
            return;
        const auto f = entries[row];
        if (row < numFolders)
        {
            setFolder (f);
            return;
        }
        processor.stopFilePreview();
        if (onLoad)
            onLoad (f);
    }

    bool SampleBrowser::jumpToLetter (juce::juce_wchar c)
    {
        if (entries.isEmpty())
            return false;
        const auto lower = juce::CharacterFunctions::toLowerCase (c);
        const int from = juce::jmax (0, list.getSelectedRow());
        for (int k = 1; k <= entries.size(); ++k)
        {
            const int row = (from + k) % entries.size();
            if (juce::CharacterFunctions::toLowerCase (entries[row].getFileName()[0]) == lower)
            {
                list.selectRow (row);
                list.scrollToEnsureRowIsOnscreen (row);
                return true;
            }
        }
        return true;   // a letter with no match is still ours, not a pad key
    }

    juce::var SampleBrowser::getDragSourceDescription (const juce::SparseSet<int>& rows)
    {
        if (rows.size() == 1 && rows[0] >= numFolders && rows[0] < entries.size())
            return "file:" + entries[rows[0]].getFullPathName();
        return {};
    }

    void SampleBrowser::paint (juce::Graphics& g)
    {
        juce::ignoreUnused (g);
    }

    void SampleBrowser::resized()
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (28);
        upButton.setBounds (top.removeFromLeft (120));
        top.removeFromLeft (6);
        chooseButton.setBounds (top.removeFromLeft (130));
        top.removeFromLeft (6);
        previewToggle.setBounds (top.removeFromRight (100));
        area.removeFromTop (4);
        folderLabel.setBounds (area.removeFromTop (20));
        list.setBounds (area);
    }
}
