#include "ParameterPanel.h"

namespace asampler
{
    namespace
    {
        constexpr int kCellWidth   = 108;
        constexpr int kCellHeight  = 104;
        constexpr int kCellGap     = 6;
        constexpr int kNameHeight  = 18;
        constexpr int kBoxHeight   = 24;
        constexpr int kTextHeight  = 18;
        constexpr int kHeaderHeight = 28;

        // The panel's section for a control. Same as the Focus group's, except
        // that the Layers section (some 70 settings) is split into the layer-wide
        // ones and one page per layer, "Layer 3" holding "Layer 3 Gain" etc.
        juce::String panelSectionOf (int controlIndex)
        {
            const auto section = ASamplerProcessor::getPadControlSection (controlIndex);
            if (section != "Layers")
                return section;

            const auto name = ASamplerProcessor::getPadControlName (controlIndex);
            if (name.startsWith ("Layer ") && name.length() > 7
                && juce::CharacterFunctions::isDigit (name[6]) && name[7] == ' ')
                return name.substring (0, 7);
            return section;
        }

        // Any control, with the editor's edit-mode keys offered first and a
        // callback when it takes focus (so the panel can scroll it into view).
        template <typename Base>
        class KeyFirst : public Base
        {
        public:
            std::function<bool (const juce::KeyPress&)> onKeyFirst;
            std::function<void()>                       onFocus;

            bool keyPressed (const juce::KeyPress& key) override
            {
                if (onKeyFirst && onKeyFirst (key))
                    return true;
                return Base::keyPressed (key);
            }

            void focusGained (juce::Component::FocusChangeType cause) override
            {
                Base::focusGained (cause);
                if (onFocus)
                    onFocus();
            }
        };

        // A knob whose value can also be typed: Enter opens the text box, and
        // focus comes back to the knob when the box closes (JUCE otherwise
        // leaves it nowhere, which a screen reader user can't see).
        class ValueSlider final : public KeyFirst<juce::Slider>,
                                  private juce::Label::Listener
        {
        public:
            juce::RangedAudioParameter* param = nullptr;   // set when bound

            bool keyPressed (const juce::KeyPress& key) override
            {
                if (onKeyFirst && onKeyFirst (key))
                    return true;

                const auto mods = key.getModifiers();
                if (key == juce::KeyPress::returnKey && ! mods.isAnyModifierKeyDown())
                {
                    typeValue();
                    return true;
                }

                if (param != nullptr && ! mods.isCommandDown() && ! mods.isAltDown())
                {
                    // JUCE steps by the parameter's interval, which is 1 Hz on
                    // Cutoff and 0.01 st on Pitch: hundreds of presses to get
                    // anywhere. Step 1% of the range instead (Shift 10%), or
                    // exactly one notch on a stepped setting such as a note.
                    const int kc = key.getKeyCode();
                    const bool up   = kc == juce::KeyPress::rightKey || kc == juce::KeyPress::upKey;
                    const bool down = kc == juce::KeyPress::leftKey  || kc == juce::KeyPress::downKey;

                    float target = -1.0f;
                    const float now = param->convertTo0to1 ((float) getValue());
                    if (up || down)
                    {
                        float step = normalisedStep();
                        if (mods.isShiftDown())
                            step *= 10.0f;
                        target = juce::jlimit (0.0f, 1.0f, now + (up ? step : -step));
                    }
                    else if (kc == juce::KeyPress::homeKey && ! mods.isShiftDown()) target = 0.0f;
                    else if (kc == juce::KeyPress::endKey  && ! mods.isShiftDown()) target = 1.0f;

                    if (target >= 0.0f)
                    {
                        setValue (param->convertFrom0to1 (target), juce::sendNotificationSync);
                        return true;
                    }
                }
                return juce::Slider::keyPressed (key);
            }

        private:
            float normalisedStep() const
            {
                const int steps = param->getNumSteps();
                return (steps > 1 && steps <= 201) ? 1.0f / (float) (steps - 1) : 0.01f;
            }

            void typeValue()
            {
                for (auto* child : getChildren())
                    if (auto* box = dynamic_cast<juce::Label*> (child))
                    {
                        box->removeListener (this);
                        box->addListener (this);
                    }
                showTextBox();
            }

            void labelTextChanged (juce::Label*) override {}

            void editorHidden (juce::Label*, juce::TextEditor&) override
            {
                juce::Component::SafePointer<juce::Slider> safe (this);
                juce::MessageManager::callAsync ([safe]
                {
                    if (safe != nullptr && safe->isShowing())
                        safe->grabKeyboardFocus();
                });
            }
        };
    }

    // The Find setting box: a one-line text field whose Up / Down / Enter /
    // Escape belong to the search rather than the caret.
    class ParameterPanel::SearchBox final : public juce::TextEditor
    {
    public:
        explicit SearchBox (ParameterPanel& p) : panel (p)
        {
            setTitle ("Find setting");
            setDescription ("Type part of a setting's name. Up and Down choose between matches, "
                            "Enter goes to it, Escape goes back.");
            setMultiLine (false);
            setReturnKeyStartsNewLine (false);
            setEscapeAndReturnKeysConsumed (false);
            setSelectAllWhenFocused (true);
            onTextChange = [this] { panel.searchChanged(); };
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (! key.getModifiers().isAnyModifierKeyDown())
            {
                const int kc = key.getKeyCode();
                if (kc == juce::KeyPress::upKey)     { panel.moveMatch (-1);  return true; }
                if (kc == juce::KeyPress::downKey)   { panel.moveMatch (+1);  return true; }
                if (kc == juce::KeyPress::returnKey) { panel.acceptSearch(); return true; }
                if (kc == juce::KeyPress::escapeKey) { panel.endSearch (true); return true; }
            }
            // Typing changes the text here and now; JUCE's change callback
            // comes later, so the match is updated (and spoken) right away.
            const bool used = juce::TextEditor::keyPressed (key);
            panel.searchChanged();
            return used;
        }

        void focusLost (juce::Component::FocusChangeType cause) override
        {
            juce::TextEditor::focusLost (cause);
            // Clicking or tabbing away ends the search. Deferred: hiding a
            // component inside its own focus change is asking for trouble.
            juce::Component::SafePointer<ParameterPanel> safe (&panel);
            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr && safe->isSearching() && ! safe->getSearchBox().hasKeyboardFocus (true))
                    safe->endSearch (false);
            });
        }

    private:
        ParameterPanel& panel;
    };

    // One setting: its visible name (for sight; the control carries the same
    // name as its accessible title) and the control bound to the parameter.
    struct ParameterPanel::Cell
    {
        enum class Kind { Knob, Choice, Toggle };

        int  controlIndex = 0;
        Kind kind         = Kind::Knob;

        juce::Label                       name;
        std::unique_ptr<juce::Component>  control;

        std::unique_ptr<juce::SliderParameterAttachment>   knobAttachment;
        std::unique_ptr<juce::ComboBoxParameterAttachment> choiceAttachment;
        std::unique_ptr<juce::ButtonParameterAttachment>   toggleAttachment;

        void unbind()
        {
            knobAttachment.reset();
            choiceAttachment.reset();
            toggleAttachment.reset();
        }
    };

    ParameterPanel::ParameterPanel (ASamplerProcessor& proc)
        : processor (proc)
    {
        for (int i = 0; i < ASamplerProcessor::getNumPadControls(); ++i)
            sections.addIfNotAlreadyThere (panelSectionOf (i));

        heading.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        heading.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (heading);

        sectionBox.setTitle ("Settings section");
        sectionBox.setDescription ("Which group of the selected pad's settings to show below.");
        sectionBox.addItemList (sections, 1);
        sectionBox.setSelectedItemIndex (0, juce::dontSendNotification);
        sectionBox.onChange = [this] { showSection (sectionBox.getSelectedItemIndex()); };
        addAndMakeVisible (sectionBox);

        searchBox = std::make_unique<SearchBox> (*this);
        addChildComponent (*searchBox);

        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        addChildComponent (graph);

        rebuild();
    }

    ParameterPanel::~ParameterPanel()
    {
        searchBox->onTextChange = nullptr;
        // Attachments first: they reference the controls.
        for (auto& cell : cells)
            cell->unbind();
    }

    void ParameterPanel::setPad (int padIndex)
    {
        padIndex = juce::jlimit (0, kNumPads - 1, padIndex);
        if (padIndex == pad)
            return;
        pad = padIndex;
        refreshHeading();
        bind();
        graph.setPadAndKind (pad, graph.getKind());
    }

    void ParameterPanel::showSection (int index)
    {
        index = juce::jlimit (0, sections.size() - 1, index);
        if (sectionBox.getSelectedItemIndex() != index)
            sectionBox.setSelectedItemIndex (index, juce::dontSendNotification);
        if (index == sectionIndex && ! cells.empty())
            return;
        sectionIndex = index;
        rebuild();
    }

    // ---- Find a setting ----

    std::vector<int> ParameterPanel::findSettings (const juce::String& query)
    {
        const auto q = query.trim().toLowerCase();
        std::vector<int> result;
        if (q.isEmpty())
            return result;

        juce::StringArray tokens;
        tokens.addTokens (q, " ", "");
        tokens.removeEmptyStrings();

        std::vector<std::pair<int, int>> ranked;   // rank, control index
        for (int i = 0; i < ASamplerProcessor::getNumPadControls(); ++i)
        {
            const auto name = ASamplerProcessor::getPadControlName (i).toLowerCase();
            juce::StringArray words;
            words.addTokens (name, " -/>", "");
            words.removeEmptyStrings();

            auto startsAWord = [&words] (const juce::String& t)
            {
                for (const auto& w : words)
                    if (w.startsWith (t))
                        return true;
                return false;
            };

            int rank = -1;
            if (name.startsWith (q))
                rank = 0;
            else
            {
                bool all = ! tokens.isEmpty();
                for (const auto& t : tokens)
                    all = all && startsAWord (t);
                if (all)
                    rank = 1;
                else if (name.contains (q))
                    rank = 2;
            }
            if (rank >= 0)
                ranked.emplace_back (rank, i);
        }

        std::stable_sort (ranked.begin(), ranked.end(),
                          [] (const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& r : ranked)
            result.push_back (r.second);
        return result;
    }

    juce::TextEditor& ParameterPanel::getSearchBox() noexcept { return *searchBox; }

    bool ParameterPanel::isSearching() const noexcept { return searchBox->isVisible(); }

    void ParameterPanel::startSearch()
    {
        if (! isSearching())
        {
            auto* focused = juce::Component::getCurrentlyFocusedComponent();
            focusBeforeSearch = (focused != nullptr && focused != searchBox.get()) ? focused : nullptr;
            matches.clear();
            matchIndex = 0;
            lastQuery.clear();
            searchBox->setText ({}, false);
            searchBox->setVisible (true);
            heading.setVisible (false);
            resized();
        }
        searchBox->grabKeyboardFocus();
    }

    void ParameterPanel::searchChanged()
    {
        // Called on each key and again by the deferred change callback: only
        // a real change of text starts a new search.
        if (searchBox->getText() == lastQuery)
            return;
        lastQuery = searchBox->getText();
        matches = findSettings (lastQuery);
        matchIndex = 0;
        if (searchBox->getText().trim().isNotEmpty())
            announceMatch();
    }

    void ParameterPanel::moveMatch (int delta)
    {
        if (matches.empty())
        {
            announceMatch();
            return;
        }
        const int n = (int) matches.size();
        matchIndex = ((matchIndex + delta) % n + n) % n;
        announceMatch();
    }

    void ParameterPanel::announceMatch()
    {
        if (! onAnnounce)
            return;
        if (matches.empty())
        {
            onAnnounce ("No setting matches " + searchBox->getText().trim());
            return;
        }
        const int index = matches[(std::size_t) matchIndex];
        onAnnounce (ASamplerProcessor::getPadControlName (index) + ", " + panelSectionOf (index)
                    + ", " + juce::String (matchIndex + 1) + " of " + juce::String ((int) matches.size()));
    }

    void ParameterPanel::acceptSearch()
    {
        if (matches.empty())
        {
            announceMatch();
            return;
        }
        const int index = matches[(std::size_t) matchIndex];
        endSearch (false);
        showSection (sections.indexOf (panelSectionOf (index)));
        for (auto& cell : cells)
            if (cell->controlIndex == index)
            {
                // Focus speaks the control: its name and value.
                cell->control->grabKeyboardFocus();
                return;
            }
    }

    void ParameterPanel::endSearch (bool restoreFocus)
    {
        if (! isSearching())
            return;
        searchBox->setVisible (false);
        heading.setVisible (true);
        resized();

        if (restoreFocus)
        {
            if (auto* back = focusBeforeSearch.getComponent(); back != nullptr && back->isShowing())
                back->grabKeyboardFocus();
            else
                sectionBox.grabKeyboardFocus();
        }
        focusBeforeSearch = nullptr;
    }

    void ParameterPanel::refreshHeading()
    {
        auto text = "Pad " + juce::String (pad + 1) + " settings";
        const auto name = processor.getPadName (pad);
        if (name.isNotEmpty())
            text = "Pad " + juce::String (pad + 1) + ", " + name + ", settings";
        heading.setText (text, juce::dontSendNotification);
        heading.setTitle (text);
    }

    void ParameterPanel::rebuild()
    {
        for (auto& cell : cells)
            cell->unbind();
        cells.clear();

        const auto& section = sections[sectionIndex];

        for (int i = 0; i < ASamplerProcessor::getNumPadControls(); ++i)
        {
            if (panelSectionOf (i) != section)
                continue;

            auto* param = processor.getPadControlParameter (pad, i);
            if (param == nullptr)
                continue;

            auto cell = std::make_unique<Cell>();
            cell->controlIndex = i;
            const auto label   = ASamplerProcessor::getPadControlName (i);

            auto wire = [this] (auto& c)
            {
                c.onKeyFirst = [this] (const juce::KeyPress& k) { return onKeyFirst && onKeyFirst (k); };
                c.onFocus    = [this, ptr = &c] { scrollToShow (*ptr); };
            };

            if (param->isBoolean())
            {
                cell->kind = Cell::Kind::Toggle;
                auto toggle = std::make_unique<KeyFirst<juce::ToggleButton>>();
                toggle->setButtonText (label);
                wire (*toggle);
                cell->control = std::move (toggle);
            }
            else if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
            {
                cell->kind = Cell::Kind::Choice;
                auto box = std::make_unique<KeyFirst<juce::ComboBox>>();
                box->addItemList (choice->choices, 1);
                wire (*box);
                cell->control = std::move (box);
            }
            else
            {
                cell->kind = Cell::Kind::Knob;
                auto knob = std::make_unique<ValueSlider>();
                knob->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, kCellWidth, kTextHeight);
                knob->setTextBoxIsEditable (true);
                knob->setDescription ("Arrow keys change it; Enter to type a value.");
                wire (*knob);
                cell->control = std::move (knob);
            }

            cell->control->setTitle (label);
            cell->control->setWantsKeyboardFocus (true);
            content.addAndMakeVisible (cell->control.get());

            // The visible name is for sight only; the control already carries
            // it as its title, and reading it twice is noise.
            if (cell->kind != Cell::Kind::Toggle)
            {
                cell->name.setText (label, juce::dontSendNotification);
                cell->name.setFont (juce::Font (juce::FontOptions (12.0f)));
                cell->name.setJustificationType (juce::Justification::centred);
                cell->name.setMinimumHorizontalScale (0.7f);
                cell->name.setAccessible (false);
                cell->name.setInterceptsMouseClicks (false, false);
                content.addAndMakeVisible (cell->name);
            }

            cells.push_back (std::move (cell));
        }

        refreshHeading();
        bind();

        const auto kind = SectionGraph::kindForSection (section);
        graph.setPadAndKind (pad, kind);
        graph.setVisible (kind != SectionGraph::Kind::None);
        resized();
    }

    void ParameterPanel::bind()
    {
        auto* undo = &processor.getUndoManager();

        for (auto& cell : cells)
        {
            cell->unbind();
            auto* param = processor.getPadControlParameter (pad, cell->controlIndex);
            if (param == nullptr)
                continue;

            switch (cell->kind)
            {
                case Cell::Kind::Knob:
                {
                    auto& knob = static_cast<ValueSlider&> (*cell->control);
                    knob.param = param;
                    cell->knobAttachment = std::make_unique<juce::SliderParameterAttachment> (*param, knob, undo);
                    knob.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
                    knob.updateText();   // the new pad's formatter ("1.250 s" in its sample)
                    break;
                }
                case Cell::Kind::Choice:
                    cell->choiceAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (
                        *param, static_cast<juce::ComboBox&> (*cell->control), undo);
                    break;
                case Cell::Kind::Toggle:
                    cell->toggleAttachment = std::make_unique<juce::ButtonParameterAttachment> (
                        *param, static_cast<juce::Button&> (*cell->control), undo);
                    break;
            }
        }
    }

    juce::RangedAudioParameter* ParameterPanel::getFocusedParameter() const
    {
        for (const auto& cell : cells)
            if (cell->control->hasKeyboardFocus (true))
                return processor.getPadControlParameter (pad, cell->controlIndex);
        return nullptr;
    }

    juce::Component* ParameterPanel::findControl (const juce::String& name) const
    {
        for (const auto& cell : cells)
            if (cell->control->getTitle() == name)
                return cell->control.get();
        return nullptr;
    }

    void ParameterPanel::paint (juce::Graphics& g)
    {
        g.setColour (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId).brighter (0.06f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    }

    void ParameterPanel::resized()
    {
        auto area = getLocalBounds().reduced (8);

        auto top = area.removeFromTop (kHeaderHeight);
        sectionBox.setBounds (top.removeFromRight (juce::jmin (180, top.getWidth() / 2)));
        heading.setBounds (top);
        searchBox->setBounds (top.reduced (0, 2).withTrimmedRight (6));
        area.removeFromTop (8);

        if (graph.isVisible())
        {
            graph.setBounds (area.removeFromTop (juce::jmin (96, area.getHeight() / 3)));
            area.removeFromTop (8);
        }

        viewport.setBounds (area);
        layoutCells();
    }

    void ParameterPanel::layoutCells()
    {
        const int width   = juce::jmax (kCellWidth, viewport.getMaximumVisibleWidth());
        const int columns = juce::jmax (1, (width + kCellGap) / (kCellWidth + kCellGap));
        const int rows    = ((int) cells.size() + columns - 1) / columns;

        content.setSize (width, juce::jmax (1, rows * (kCellHeight + kCellGap)));

        // Row by row, left to right: the Tab order follows the same path.
        for (int i = 0; i < (int) cells.size(); ++i)
        {
            auto& cell = *cells[(std::size_t) i];
            auto  r    = juce::Rectangle<int> ((i % columns) * (kCellWidth + kCellGap),
                                               (i / columns) * (kCellHeight + kCellGap),
                                               kCellWidth, kCellHeight);
            switch (cell.kind)
            {
                case Cell::Kind::Knob:
                    cell.name.setBounds (r.removeFromTop (kNameHeight));
                    cell.control->setBounds (r);
                    break;
                case Cell::Kind::Choice:
                    cell.name.setBounds (r.removeFromTop (kNameHeight));
                    cell.control->setBounds (r.removeFromTop (kBoxHeight).reduced (2, 0));
                    break;
                case Cell::Kind::Toggle:
                    r.removeFromTop (kNameHeight);
                    cell.control->setBounds (r.removeFromTop (kBoxHeight * 2).reduced (2, 0));
                    break;
            }
        }
    }

    void ParameterPanel::scrollToShow (juce::Component& c)
    {
        const auto bounds = c.getBoundsInParent();
        auto view = viewport.getViewArea();

        if (bounds.getY() < view.getY())
            viewport.setViewPosition (0, bounds.getY());
        else if (bounds.getBottom() > view.getBottom())
            viewport.setViewPosition (0, bounds.getBottom() - view.getHeight());
    }
}
