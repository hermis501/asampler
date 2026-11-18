#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "SectionGraph.h"

#include <functional>
#include <memory>
#include <vector>

namespace asampler
{
    // The selected pad's settings, inside the editor.
    //
    // REAPER lists every host parameter in its FX window, so there the Focus
    // group is enough. Most hosts have nothing like it: in Studio One, Live or
    // Logic the only way to a pad's filter or envelope would be an automation
    // lane. This panel puts the same controls in the plugin's own window, so
    // ASampler works the same in any host, with or without sight.
    //
    // One section at a time (the Focus group's sections: Main, Trigger,
    // Slice, ...), chosen from a combo box, so the Tab order stays short.
    // Controls are JUCE's own Slider (drawn as a knob), ComboBox and
    // ToggleButton, which screen readers read and change like any native
    // control, and each is bound to the selected pad's real parameter: values
    // read exactly as in the host list ("-6.00 dB", "1.250 s"), and a change
    // is a normal undoable, automatable parameter change.
    //
    // Moving to another pad only rebinds the controls; they stay where they
    // are, so a focused knob keeps its focus. Enter on a knob opens its text
    // box to type a value ("480 ms"); Escape or Enter closes it again.
    class ParameterPanel : public juce::Component
    {
    public:
        explicit ParameterPanel (ASamplerProcessor& proc);
        ~ParameterPanel() override;

        // Points every control at `padIndex`'s parameters. Cheap when the pad
        // hasn't changed, so the editor can call it from its timer.
        void setPad (int padIndex);
        int  getPad() const noexcept { return pad; }

        // Sections in Focus-group order.
        const juce::StringArray& getSections() const noexcept { return sections; }
        void showSection (int index);
        int  getSectionIndex() const noexcept { return sectionIndex; }

        // Offered every key before a control handles it, so the editor's edit
        // modes (T / O) keep the arrows even while a knob has focus.
        std::function<bool (const juce::KeyPress&)> onKeyFirst;

        // ---- Find a setting (Ctrl+F) ----
        //
        // A text box over the heading. Typing names a setting in any section
        // ("cutoff", "env rel", "lfo 2 rate"); the best match is spoken, Up
        // and Down move between matches, Enter shows its section and focuses
        // it, Escape goes back to where focus was. Plain letters can't do
        // this: on the panel they already play pads and run shortcuts.
        void startSearch();
        bool isSearching() const noexcept;
        // Spoken feedback (the editor routes it to the screen reader).
        std::function<void (const juce::String&)> onAnnounce;

        // Pad control indices matching `query`, best first: the name starts
        // with it, then every word of it starts a word of the name, then the
        // name contains it.
        static std::vector<int> findSettings (const juce::String& query);

        // ---- For tests ----
        SectionGraph& getGraph() noexcept { return graph; }
        // The control for the named setting in the current section ("Cutoff"),
        // or nullptr.
        juce::Component* findControl (const juce::String& name) const;
        juce::ComboBox&  getSectionBox() noexcept { return sectionBox; }
        juce::TextEditor& getSearchBox() noexcept;

        // The pad parameter whose control has keyboard focus, or nullptr.
        juce::RangedAudioParameter* getFocusedParameter() const;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        struct Cell;
        class SearchBox;

        void searchChanged();
        void moveMatch (int delta);
        void acceptSearch();
        void endSearch (bool restoreFocus);
        void announceMatch();

        void rebuild();
        void bind();
        void layoutCells();
        void scrollToShow (juce::Component& c);
        void refreshHeading();

        ASamplerProcessor& processor;

        juce::Label    heading;
        juce::ComboBox sectionBox;
        juce::Viewport viewport;
        juce::Component content;
        SectionGraph    graph { processor };   // envelope / filter / LFO picture

        juce::StringArray                  sections;
        std::vector<std::unique_ptr<Cell>> cells;

        int pad          = 0;
        int sectionIndex = 0;

        std::unique_ptr<SearchBox>               searchBox;
        std::vector<int>                         matches;
        int                                      matchIndex = 0;
        juce::String                             lastQuery;
        juce::Component::SafePointer<juce::Component> focusBeforeSearch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterPanel)
    };
}
