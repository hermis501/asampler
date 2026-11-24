#include "WaveformView.h"
#include "engine/LoopMode.h"
#include "engine/Sample.h"

namespace asampler
{
    namespace
    {
        constexpr float kMarkerGrab = 6.0f;    // pixels either side of a marker
        constexpr float kMinGap     = 0.0005f; // Start stays this far before End

        const juce::Colour kWave      { 0xff5fb0e8 };
        const juce::Colour kWaveDim   { 0xff3a5f7a };
        const juce::Colour kStartCol  { 0xff6ad48a };
        const juce::Colour kEndCol    { 0xffe86a6a };
        const juce::Colour kLoopCol   { 0xffe8c45f };
        const juce::Colour kPlayCol   { 0xffffffff };
        const juce::Colour kSibling   { 0x99ffffff };

        juce::String markerName (int m)
        {
            switch (m)
            {
                case 0:  return "Start";
                case 1:  return "End";
                case 2:  return "Loop Start";
                default: return "Loop End";
            }
        }
    }

    WaveformView::WaveformView (ASamplerProcessor& proc) : processor (proc)
    {
        setWantsKeyboardFocus (false);
        setAccessible (false);   // see the header: nothing here the keyboard can't reach
        setInterceptsMouseClicks (true, false);
        setPad (processor.getCurrentFocusedPad());
    }

    WaveformView::~WaveformView() = default;

    void WaveformView::setPad (int padIndex)
    {
        if (! juce::isPositiveAndBelow (padIndex, kNumPads))
            return;
        if (padIndex == pad && attachments[kStart] != nullptr)
            return;

        pad = padIndex;
        dragging = -1;
        bindMarkers();
        sample.reset();   // forces the peaks to rebuild
        siblingCountdown = 0;
        tick();
        repaint();
    }

    void WaveformView::bindMarkers()
    {
        const int p = pad + 1;
        const juce::String ids[kNumMarkers] = { param_ids::padStart (p), param_ids::padEnd (p),
                                                param_ids::padLoopStart (p), param_ids::padLoopEnd (p) };

        for (int m = 0; m < kNumMarkers; ++m)
        {
            attachments[(std::size_t) m].reset();
            markerParams[(std::size_t) m] = processor.getState().getParameter (ids[m]);
            if (auto* param = markerParams[(std::size_t) m])
                attachments[(std::size_t) m] = std::make_unique<juce::ParameterAttachment> (
                    *param, [this] (float) { repaint(); }, &processor.getUndoManager());
        }
    }

    float WaveformView::markerValue (int m) const
    {
        if (auto* param = markerParams[(std::size_t) m])
            return param->convertFrom0to1 (param->getValue());
        return m == kEnd || m == kLoopEnd ? 1.0f : 0.0f;
    }

    bool WaveformView::markerVisible (int m) const
    {
        if (m == kStart || m == kEnd)
            return true;
        auto* raw = processor.getState().getRawParameterValue (param_ids::padLoopMode (pad + 1));
        return raw != nullptr && juce::roundToInt (raw->load()) != static_cast<int> (LoopMode::Off);
    }

    void WaveformView::rebuildPeaks()
    {
        peakMin.clear();
        peakMax.clear();
        if (sample == nullptr || sample->isEmpty())
            return;

        const int n      = sample->getNumSamples();
        const int chans  = sample->getNumChannels();
        const int blocks = (n + kBlock - 1) / kBlock;
        peakMin.assign ((std::size_t) blocks, 0.0f);
        peakMax.assign ((std::size_t) blocks, 0.0f);

        for (int b = 0; b < blocks; ++b)
        {
            const int s0 = b * kBlock;
            const int len = juce::jmin (kBlock, n - s0);
            float lo = 0.0f, hi = 0.0f;
            for (int ch = 0; ch < chans; ++ch)
            {
                const auto range = juce::FloatVectorOperations::findMinAndMax (sample->getReadPointer (ch) + s0, len);
                lo = juce::jmin (lo, range.getStart());
                hi = juce::jmax (hi, range.getEnd());
            }
            peakMin[(std::size_t) b] = lo;
            peakMax[(std::size_t) b] = hi;
        }
    }

    void WaveformView::refreshSiblings()
    {
        siblings.clear();
        const auto file = processor.getPadSampleFile (pad);
        if (file == juce::File())
            return;
        for (int i = 0; i < kNumPads; ++i)
            if (i != pad && processor.getPadSampleFile (i) == file)
                if (auto* raw = processor.getState().getRawParameterValue (param_ids::padStart (i + 1)))
                    siblings.emplace_back (raw->load(), i + 1);
    }

    void WaveformView::tick()
    {
        bool changed = false;

        if (auto current = processor.getPadDisplaySample (pad); current != sample)
        {
            sample = std::move (current);
            rebuildPeaks();
            viewStart = 0.0;
            viewEnd   = 1.0;
            siblingCountdown = 0;
            changed = true;
        }

        // Other pads' slices change rarely; a few times a second is plenty.
        if (--siblingCountdown <= 0)
        {
            const auto before = siblings;
            refreshSiblings();
            siblingCountdown = 15;
            changed = changed || siblings != before;
        }

        for (int m = 0; m < kNumMarkers; ++m)
        {
            const float v = markerValue (m);
            if (v != shownMarkers[(std::size_t) m]) { shownMarkers[(std::size_t) m] = v; changed = true; }
        }
        const bool loop = markerVisible (kLoopStart);
        if (loop != shownLoop) { shownLoop = loop; changed = true; }

        const int editLayer = processor.getCurrentEditLayer();
        if (editLayer != shownEditLayer) { shownEditLayer = editLayer; changed = true; }

        const float play = processor.getPadPlayFraction (pad);
        if (std::abs (play - shownPlayhead) > 0.0005f) { shownPlayhead = play; changed = true; }

        if (changed)
            repaint();
    }

    juce::Rectangle<float> WaveformView::waveArea() const
    {
        return getLocalBounds().toFloat().reduced (4.0f, 2.0f).withTrimmedTop (16.0f);
    }

    float WaveformView::xForPosition (double norm) const
    {
        const auto area = waveArea();
        const double span = juce::jmax (1.0e-9, viewEnd - viewStart);
        return area.getX() + (float) ((norm - viewStart) / span) * area.getWidth();
    }

    double WaveformView::positionForX (float x) const
    {
        const auto area = waveArea();
        const double t = area.getWidth() > 0.0f ? (x - area.getX()) / area.getWidth() : 0.0;
        return juce::jlimit (0.0, 1.0, viewStart + t * (viewEnd - viewStart));
    }

    int WaveformView::markerAt (float x) const
    {
        int best = -1;
        float bestDist = kMarkerGrab;
        for (int m = 0; m < kNumMarkers; ++m)
        {
            if (! markerVisible (m)) continue;
            const float d = std::abs (xForPosition (markerValue (m)) - x);
            if (d <= bestDist) { bestDist = d; best = m; }
        }
        return best;
    }

    void WaveformView::zoom (double factor, double around)
    {
        const double span = juce::jlimit (1.0 / 2000.0, 1.0, (viewEnd - viewStart) * factor);
        double start = around - (around - viewStart) * (span / juce::jmax (1.0e-9, viewEnd - viewStart));
        start = juce::jlimit (0.0, 1.0 - span, start);
        viewStart = start;
        viewEnd   = start + span;
        repaint();
    }

    void WaveformView::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff15191e));
        g.fillRoundedRectangle (bounds, 4.0f);

        const auto area = waveArea();
        const auto content = processor.getPadContent (pad);

        // Title line: what is shown, so the picture explains itself.
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.setFont (juce::FontOptions (12.0f));
        auto name = processor.getPadSampleDisplayName (pad);
        if (name == "default") name = "built-in tone";
        juce::String title = "Pad " + juce::String (pad + 1) + "  " + name;
        if (sample != nullptr && sample->getSourceSampleRate() > 0.0)
            title << "  " << juce::String (sample->getNumSamples() / sample->getSourceSampleRate(), 2) << " s";
        if (shownEditLayer > 0)
            title << "  |  Load goes to layer " << (shownEditLayer + 1);
        if (viewEnd - viewStart < 0.999)
            title << "  (zoomed, double-click to show all)";
        g.drawText (title, bounds.reduced (6.0f, 0.0f).withHeight (16.0f), juce::Justification::centredLeft, true);

        if (content == ASamplerProcessor::PadContent::Missing)
        {
            g.setColour (kEndCol);
            g.drawText ("Sample file missing. Use Relink samples to find it.", area, juce::Justification::centred);
            return;
        }
        if (sample == nullptr || peakMax.empty())
        {
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.drawText ("No sample", area, juce::Justification::centred);
            return;
        }

        const float start = juce::jmin (shownMarkers[kStart], shownMarkers[kEnd]);
        const float end   = juce::jmax (shownMarkers[kStart], shownMarkers[kEnd]);

        // Shade what lies outside the played window.
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        const float xs = juce::jlimit (area.getX(), area.getRight(), xForPosition (start));
        const float xe = juce::jlimit (area.getX(), area.getRight(), xForPosition (end));
        g.fillRect (juce::Rectangle<float> (area.getX(), area.getY(), xs - area.getX(), area.getHeight()));
        g.fillRect (juce::Rectangle<float> (xe, area.getY(), area.getRight() - xe, area.getHeight()));

        // The waveform, one min/max line per pixel column.
        const int    n     = sample->getNumSamples();
        const float  midY  = area.getCentreY();
        const float  halfH = area.getHeight() * 0.5f;
        const int    cols  = juce::jmax (1, (int) area.getWidth());
        const double perCol = (viewEnd - viewStart) * n / cols;

        for (int c = 0; c < cols; ++c)
        {
            const double a = (viewStart * n) + c * perCol;
            const int s0 = juce::jlimit (0, n - 1, (int) a);
            const int s1 = juce::jlimit (s0 + 1, n, (int) (a + perCol) + 1);
            float lo = 0.0f, hi = 0.0f;

            if (s1 - s0 >= kBlock)
            {
                const int b0 = s0 / kBlock, b1 = juce::jmin ((int) peakMax.size(), (s1 + kBlock - 1) / kBlock);
                for (int b = b0; b < b1; ++b)
                {
                    lo = juce::jmin (lo, peakMin[(std::size_t) b]);
                    hi = juce::jmax (hi, peakMax[(std::size_t) b]);
                }
            }
            else
            {
                for (int ch = 0; ch < sample->getNumChannels(); ++ch)
                {
                    const auto r = juce::FloatVectorOperations::findMinAndMax (sample->getReadPointer (ch) + s0, s1 - s0);
                    lo = juce::jmin (lo, r.getStart());
                    hi = juce::jmax (hi, r.getEnd());
                }
            }

            const double pos = (double) s0 / n;
            g.setColour (pos >= start && pos <= end ? kWave : kWaveDim);
            const float x = area.getX() + (float) c;
            const float y0 = midY - juce::jlimit (-1.0f, 1.0f, hi) * halfH;
            const float y1 = midY - juce::jlimit (-1.0f, 1.0f, lo) * halfH;
            g.drawVerticalLine ((int) x, y0, juce::jmax (y0 + 1.0f, y1));
        }

        // Other pads' slices of the same file.
        g.setFont (juce::FontOptions (10.0f));
        for (const auto& [pos, number] : siblings)
        {
            const float x = xForPosition (pos);
            if (x < area.getX() || x > area.getRight()) continue;
            g.setColour (kSibling);
            g.drawVerticalLine ((int) x, area.getBottom() - 14.0f, area.getBottom());
            g.drawText (juce::String (number), juce::Rectangle<float> (x + 2.0f, area.getBottom() - 12.0f, 20.0f, 12.0f),
                        juce::Justification::centredLeft, false);
        }

        // Markers.
        const juce::Colour colours[kNumMarkers] = { kStartCol, kEndCol, kLoopCol, kLoopCol };
        const char* tags[kNumMarkers] = { "S", "E", "L", "L" };
        for (int m = 0; m < kNumMarkers; ++m)
        {
            if (m >= kLoopStart && ! shownLoop) continue;
            const float x = xForPosition (shownMarkers[(std::size_t) m]);
            if (x < area.getX() - 1.0f || x > area.getRight() + 1.0f) continue;
            g.setColour (colours[m].withAlpha (dragging == m ? 1.0f : 0.85f));
            g.fillRect (juce::Rectangle<float> (x - 1.0f, area.getY(), 2.0f, area.getHeight()));
            const float tagX = (m == kEnd || m == kLoopEnd) ? x - 13.0f : x + 1.0f;
            g.fillRect (juce::Rectangle<float> (tagX, area.getY(), 12.0f, 12.0f));
            g.setColour (juce::Colours::black);
            g.drawText (tags[m], juce::Rectangle<float> (tagX, area.getY(), 12.0f, 12.0f),
                        juce::Justification::centred, false);
        }

        // Playhead.
        if (shownPlayhead >= 0.0f)
        {
            const float x = xForPosition (shownPlayhead);
            if (x >= area.getX() && x <= area.getRight())
            {
                g.setColour (kPlayCol);
                g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            }
        }
    }

    void WaveformView::mouseMove (const juce::MouseEvent& e)
    {
        const int m = markerAt ((float) e.x);
        setMouseCursor (m >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        setTooltip (m >= 0 ? "Drag to move " + markerName (m) + ". Double-click resets it."
                           : juce::String ("Drag a marker to move it. Mouse wheel zooms."));
    }

    void WaveformView::mouseDown (const juce::MouseEvent& e)
    {
        dragging = sample != nullptr ? markerAt ((float) e.x) : -1;
        if (dragging >= 0)
            if (auto& a = attachments[(std::size_t) dragging])
                a->beginGesture();
    }

    void WaveformView::mouseDrag (const juce::MouseEvent& e)
    {
        if (dragging < 0)
            return;
        auto& a = attachments[(std::size_t) dragging];
        if (a == nullptr)
            return;

        float v = (float) positionForX ((float) e.x);
        // Start stays before End, and Loop Start before Loop End, so a drag
        // can never produce a window that plays nothing.
        if (dragging == kStart)     v = juce::jmin (v, markerValue (kEnd) - kMinGap);
        if (dragging == kEnd)       v = juce::jmax (v, markerValue (kStart) + kMinGap);
        if (dragging == kLoopStart) v = juce::jmin (v, markerValue (kLoopEnd) - kMinGap);
        if (dragging == kLoopEnd)   v = juce::jmax (v, markerValue (kLoopStart) + kMinGap);
        a->setValueAsPartOfGesture (juce::jlimit (0.0f, 1.0f, v));
        tick();
    }

    void WaveformView::mouseUp (const juce::MouseEvent&)
    {
        if (dragging >= 0)
            if (auto& a = attachments[(std::size_t) dragging])
                a->endGesture();
        dragging = -1;
        repaint();
    }

    void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const int m = markerAt ((float) e.x);
        if (m < 0)
        {
            viewStart = 0.0;
            viewEnd   = 1.0;
            repaint();
            return;
        }
        if (auto& a = attachments[(std::size_t) m])
        {
            float def = (m == kEnd || m == kLoopEnd) ? 1.0f : 0.0f;
            if (auto* param = markerParams[(std::size_t) m])
                def = param->convertFrom0to1 (param->getDefaultValue());
            if (m == kStart)     def = juce::jmin (def, markerValue (kEnd) - kMinGap);
            if (m == kEnd)       def = juce::jmax (def, markerValue (kStart) + kMinGap);
            if (m == kLoopStart) def = juce::jmin (def, markerValue (kLoopEnd) - kMinGap);
            if (m == kLoopEnd)   def = juce::jmax (def, markerValue (kLoopStart) + kMinGap);
            a->setValueAsCompleteGesture (juce::jlimit (0.0f, 1.0f, def));
        }
    }

    void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        if (sample == nullptr || wheel.deltaY == 0.0f)
            return;
        zoom (wheel.deltaY > 0 ? 0.8 : 1.25, positionForX ((float) e.x));
    }
}
