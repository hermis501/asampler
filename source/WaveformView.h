#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

#include <array>
#include <memory>
#include <vector>

namespace asampler
{
    // The selected pad's sample, drawn, with its Start / End (and loop)
    // markers and a playhead while it sounds.
    //
    // A picture for sighted users only: everything it shows and does is
    // reachable without it (Start and End are host parameters and settings
    // knobs, T / O place them by ear, Describe speaks what the waveform
    // shows). So it takes no keyboard focus and stays out of the
    // accessibility tree rather than adding a stop with nothing to say.
    //
    // Mouse: drag a marker to move it (an undoable parameter gesture, so the
    // host records it like any knob), double-click one to reset it, the wheel
    // zooms around the pointer, double-click elsewhere shows the whole
    // sample again. Slices of the same file on other pads show as thin
    // numbered marks, so a chop can be seen at a glance.
    class WaveformView : public juce::Component,
                         public juce::SettableTooltipClient
    {
    public:
        explicit WaveformView (ASamplerProcessor& proc);
        ~WaveformView() override;

        void setPad (int padIndex);
        int  getPad() const noexcept { return pad; }

        // Called by the editor once per frame: picks up a new sample, moved
        // markers and the playhead, and repaints only when something changed.
        void tick();

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        // ---- For tests ----
        int  getPeakBlockCount() const noexcept { return (int) peakMax.size(); }
        // The marker index (0 Start, 1 End, 2 Loop Start, 3 Loop End) under x,
        // or -1.
        int  markerAt (float x) const;
        float xForPosition (double norm) const;
        double positionForX (float x) const;
        void zoom (double factor, double aroundNorm);
        double getViewStart() const noexcept { return viewStart; }
        double getViewEnd() const noexcept   { return viewEnd; }

    private:
        enum { kStart, kEnd, kLoopStart, kLoopEnd, kNumMarkers };

        void rebuildPeaks();
        void bindMarkers();
        float markerValue (int marker) const;
        bool  markerVisible (int marker) const;
        void  refreshSiblings();
        juce::Rectangle<float> waveArea() const;

        ASamplerProcessor& processor;
        int pad = 0;
        int shownEditLayer = 0;   // the title says where Load puts a sample

        std::shared_ptr<const Sample> sample;     // what the peaks were built from
        std::vector<float> peakMin, peakMax;      // per kBlock samples, all channels
        static constexpr int kBlock = 256;

        double viewStart = 0.0, viewEnd = 1.0;    // visible part, 0..1 of the sample

        std::array<std::unique_ptr<juce::ParameterAttachment>, kNumMarkers> attachments;
        std::array<juce::RangedAudioParameter*, kNumMarkers> markerParams {};
        int dragging = -1;

        // What was painted last, to repaint only on change.
        std::array<float, kNumMarkers> shownMarkers {};
        bool  shownLoop = false;
        float shownPlayhead = -1.0f;

        // Other pads holding the same file: their Start, and their number.
        std::vector<std::pair<float, int>> siblings;
        int siblingCountdown = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
    };
}
