#include "SectionGraph.h"
#include "engine/FilterMode.h"
#include "engine/Lfo.h"

#include <cmath>

namespace asampler
{
    namespace
    {
        const juce::Colour kBack  { 0xff15191e };
        const juce::Colour kGrid  { 0xff2a3038 };
        const juce::Colour kLine  { 0xff5fb0e8 };
        const juce::Colour kLine2 { 0xffe8c45f };
        const juce::Colour kText  { 0xccffffff };

        juce::Path envelopePath (juce::Rectangle<float> r, float a, float d, float s, float rel, float h = 0.0f)
        {
            // Times on a square-root scale, so a 2 ms attack is still visible
            // beside a 2 s release; the sustain hold gets a fixed share.
            const float ta = std::sqrt (juce::jmax (0.0f, a));
            const float td = std::sqrt (juce::jmax (0.0f, d));
            const float tr = std::sqrt (juce::jmax (0.0f, rel));
            const float th = std::sqrt (juce::jmax (0.0f, h));
            const float hold = juce::jmax (0.25f, (ta + th + td + tr) * 0.35f);
            const float total = juce::jmax (1.0e-3f, ta + th + td + hold + tr);
            const float k = r.getWidth() / total;
            const float sy = r.getBottom() - juce::jlimit (0.0f, 1.0f, s) * r.getHeight();

            juce::Path p;
            float x = r.getX();
            p.startNewSubPath (x, r.getBottom());
            x += ta * k;            p.lineTo (x, r.getY());
            x += th * k;            p.lineTo (x, r.getY());
            x += td * k;            p.lineTo (x, sy);
            x += hold * k;          p.lineTo (x, sy);
            x += tr * k;            p.lineTo (x, r.getBottom());
            return p;
        }

        // Magnitude of a 2-pole state-variable filter at `f`, in dB.
        float svfResponseDb (FilterMode mode, float f, float fc, float q)
        {
            const double w = f / juce::jmax (1.0f, fc);
            const double qq = juce::jmax (0.1f, q);
            const double denom = std::sqrt ((1.0 - w * w) * (1.0 - w * w) + (w / qq) * (w / qq));
            double mag = 1.0;
            switch (mode)
            {
                case FilterMode::LowPass:  mag = 1.0 / denom; break;
                case FilterMode::HighPass: mag = (w * w) / denom; break;
                case FilterMode::BandPass: mag = (w / qq) / denom; break;
                case FilterMode::Off:      mag = 1.0; break;
            }
            return (float) juce::Decibels::gainToDecibels (mag, -60.0);
        }
    }

    SectionGraph::SectionGraph (ASamplerProcessor& proc) : processor (proc)
    {
        setAccessible (false);   // see the header
        setWantsKeyboardFocus (false);
        setInterceptsMouseClicks (false, false);
    }

    SectionGraph::Kind SectionGraph::kindForSection (const juce::String& section)
    {
        if (section == "Envelope")   return Kind::Envelope;
        if (section == "Filter")     return Kind::Filter;
        if (section == "Modulation") return Kind::Modulation;
        return Kind::None;
    }

    void SectionGraph::setPadAndKind (int padIndex, Kind newKind)
    {
        pad  = juce::jlimit (0, kNumPads - 1, padIndex);
        kind = newKind;
        shown = snapshot();
        if (kind == Kind::None) stopTimer();
        else                    startTimerHz (15);
        repaint();
    }

    float SectionGraph::value (const juce::String& id) const
    {
        if (auto* raw = processor.getState().getRawParameterValue (id))
            return raw->load();
        return 0.0f;
    }

    std::vector<float> SectionGraph::snapshot() const
    {
        const int p = pad + 1;
        using namespace param_ids;
        switch (kind)
        {
            case Kind::Envelope:
                return { value (padAttack (p)), value (padDecay (p)), value (padSustain (p)), value (padRelease (p)),
                         value (padHold (p)) };
            case Kind::Filter:
                return { value (padFilterMode (p)), value (padFilterCutoff (p)), value (padFilterResonance (p)),
                         value (padFiltEnvAmount (p)), value (padFiltEnvAttack (p)), value (padFiltEnvDecay (p)),
                         value (padFiltEnvSustain (p)), value (padFiltEnvRelease (p)), value (padFiltEnvHold (p)) };
            case Kind::Modulation:
                return { value (padLfoShape (p)), value (padLfoDepth (p)), value (padLfoDest (p)),
                         value (padLfo2Shape (p)), value (padLfo2Depth (p)), value (padLfo2Dest (p)) };
            case Kind::None:
                break;
        }
        return {};
    }

    void SectionGraph::timerCallback()
    {
        auto now = snapshot();
        if (now != shown)
        {
            shown = std::move (now);
            repaint();
        }
    }

    void SectionGraph::paint (juce::Graphics& g)
    {
        if (kind == Kind::None || shown.empty())
            return;

        auto r = getLocalBounds().toFloat();
        g.setColour (kBack);
        g.fillRoundedRectangle (r, 4.0f);
        r = r.reduced (8.0f, 6.0f);

        switch (kind)
        {
            case Kind::Envelope:   paintEnvelope (g, r);   break;
            case Kind::Filter:     paintFilter (g, r);     break;
            case Kind::Modulation: paintModulation (g, r); break;
            case Kind::None:       break;
        }
    }

    void SectionGraph::paintEnvelope (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (kText);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("Amp envelope", r.removeFromTop (12.0f), juce::Justification::centredLeft, false);
        r.removeFromTop (2.0f);

        const auto p = envelopePath (r, shown[0], shown[1], shown[2], shown[3], shown[4]);
        juce::Path fill (p);
        fill.closeSubPath();
        g.setColour (kLine.withAlpha (0.18f));
        g.fillPath (fill);
        g.setColour (kLine);
        g.strokePath (p, juce::PathStrokeType (2.0f));
    }

    void SectionGraph::paintFilter (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto mode   = static_cast<FilterMode> (juce::roundToInt (shown[0]));
        const float fc    = shown[1];
        const float q     = shown[2];
        const float envOct = shown[3];

        g.setColour (kText);
        g.setFont (juce::FontOptions (11.0f));
        auto title = r.removeFromTop (12.0f);
        g.drawText (mode == FilterMode::Off ? "Filter off" : "Filter response", title,
                    juce::Justification::centredLeft, false);

        // The filter envelope's own shape, small, on the right.
        auto envArea = r.removeFromRight (r.getWidth() * 0.32f).reduced (6.0f, 2.0f);
        r.removeFromTop (2.0f);

        // Grid: octaves from 20 Hz to 20 kHz, 0 dB line.
        constexpr float lo = 20.0f, hi = 20000.0f, topDb = 18.0f, botDb = -36.0f;
        auto xFor = [&] (float f) { return r.getX() + r.getWidth() * std::log (f / lo) / std::log (hi / lo); };
        auto yFor = [&] (float db) { return r.getY() + r.getHeight() * (topDb - juce::jlimit (botDb, topDb, db)) / (topDb - botDb); };

        g.setColour (kGrid);
        for (float f : { 100.0f, 1000.0f, 10000.0f })
            g.drawVerticalLine ((int) xFor (f), r.getY(), r.getBottom());
        g.drawHorizontalLine ((int) yFor (0.0f), r.getX(), r.getRight());

        auto curve = [&] (float cutoff)
        {
            juce::Path p;
            const int steps = juce::jmax (2, (int) r.getWidth());
            for (int i = 0; i <= steps; ++i)
            {
                const float f = lo * std::pow (hi / lo, (float) i / steps);
                const float y = yFor (svfResponseDb (mode, f, cutoff, q));
                if (i == 0) p.startNewSubPath (xFor (f), y);
                else        p.lineTo (xFor (f), y);
            }
            return p;
        };

        if (mode != FilterMode::Off && std::abs (envOct) > 0.01f)
        {
            const float peak = juce::jlimit (lo, hi, fc * std::pow (2.0f, envOct));
            g.setColour (kLine2.withAlpha (0.7f));
            g.strokePath (curve (peak), juce::PathStrokeType (1.0f));
        }
        g.setColour (kLine);
        g.strokePath (curve (fc), juce::PathStrokeType (2.0f));

        if (mode != FilterMode::Off)
        {
            g.setColour (kText);
            g.drawText (juce::String (juce::roundToInt (fc)) + " Hz",
                        juce::Rectangle<float> (juce::jlimit (r.getX(), r.getRight() - 60.0f, xFor (fc) + 3.0f),
                                                r.getY(), 60.0f, 12.0f),
                        juce::Justification::centredLeft, false);
        }

        g.setColour (kText);
        g.drawText ("Filter env", envArea.removeFromTop (12.0f), juce::Justification::centredLeft, false);
        const auto env = envelopePath (envArea, shown[4], shown[5], shown[6], shown[7], shown[8]);
        g.setColour (std::abs (envOct) > 0.01f ? kLine2 : kGrid);
        g.strokePath (env, juce::PathStrokeType (1.5f));
    }

    void SectionGraph::paintModulation (juce::Graphics& g, juce::Rectangle<float> r)
    {
        static const char* destNames[] = { "off", "pitch", "cutoff", "volume", "pan" };

        auto one = [&] (juce::Rectangle<float> area, int lfo, float shapeV, float depth, float destV, juce::Colour c)
        {
            const int dest = juce::jlimit (0, 4, juce::roundToInt (destV));
            g.setColour (kText);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("LFO " + juce::String (lfo) + " to " + destNames[dest], area.removeFromTop (12.0f),
                        juce::Justification::centredLeft, false);
            area.removeFromTop (2.0f);

            g.setColour (kGrid);
            g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

            const auto shape = static_cast<LfoShape> (juce::jlimit (0, 3, juce::roundToInt (shapeV)));
            const float amp = dest == 0 ? 0.0f : juce::jlimit (0.0f, 1.0f, depth);
            juce::Path p;
            const int steps = juce::jmax (2, (int) area.getWidth());
            for (int i = 0; i <= steps; ++i)
            {
                const double phase = std::fmod (2.0 * i / steps, 1.0);   // two cycles
                const float v = lfoValue (shape, phase) * amp;
                const float x = area.getX() + area.getWidth() * (float) i / steps;
                const float y = area.getCentreY() - v * area.getHeight() * 0.45f;
                if (i == 0) p.startNewSubPath (x, y);
                else        p.lineTo (x, y);
            }
            g.setColour (dest == 0 ? kGrid : c);
            g.strokePath (p, juce::PathStrokeType (1.5f));
        };

        auto left = r.removeFromLeft (r.getWidth() * 0.5f).reduced (0.0f, 0.0f).withTrimmedRight (6.0f);
        one (left, 1, shown[0], shown[1], shown[2], kLine);
        one (r.withTrimmedLeft (6.0f), 2, shown[3], shown[4], shown[5], kLine2);
    }
}
