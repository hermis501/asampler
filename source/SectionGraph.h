#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

#include <vector>

namespace asampler
{
    // A small picture above the settings panel's knobs for the sections that
    // are easier to see than to read: the amplitude envelope, the filter's
    // response (with where the filter envelope takes it), and the two LFOs.
    //
    // For sight only. Every value it draws is a knob right below it, read by
    // the screen reader exactly, so it stays out of the accessibility tree.
    class SectionGraph : public juce::Component,
                         private juce::Timer
    {
    public:
        enum class Kind { None, Envelope, Filter, Modulation };

        explicit SectionGraph (ASamplerProcessor& proc);

        static Kind kindForSection (const juce::String& section);

        void setPadAndKind (int padIndex, Kind kind);
        Kind getKind() const noexcept { return kind; }

        void paint (juce::Graphics&) override;

    private:
        void timerCallback() override;
        float value (const juce::String& id) const;
        std::vector<float> snapshot() const;

        void paintEnvelope (juce::Graphics&, juce::Rectangle<float>);
        void paintFilter (juce::Graphics&, juce::Rectangle<float>);
        void paintModulation (juce::Graphics&, juce::Rectangle<float>);

        ASamplerProcessor& processor;
        int  pad  = 0;
        Kind kind = Kind::None;
        std::vector<float> shown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionGraph)
    };
}
