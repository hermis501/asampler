#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace asampler
{
    // juce::ADSR with a Hold stage between attack and decay (AHDSR): after
    // reaching full level the envelope stays there for `holdSeconds` before
    // decaying. The classic shape for 808s and gated hits, where the body
    // should stay at full level for a while and then fall away.
    //
    // With hold = 0 it is juce::ADSR, sample for sample: the hold only pauses
    // the inner ADSR at its peak, it never changes its maths.
    class HoldEnvelope
    {
    public:
        void setSampleRate (double sr) noexcept
        {
            sampleRate = sr > 0.0 ? sr : 44100.0;
            adsr.setSampleRate (sampleRate);
        }

        void setParameters (const juce::ADSR::Parameters& p, float holdSeconds) noexcept
        {
            adsr.setParameters (p);
            holdSamples = holdSeconds > 0.0f ? (int) std::lround (holdSeconds * sampleRate) : 0;
        }

        void reset() noexcept
        {
            adsr.reset();
            holdLeft = 0;
            waitingForPeak = false;
        }

        void noteOn() noexcept
        {
            adsr.noteOn();
            holdLeft = 0;
            waitingForPeak = false;
            if (holdSamples <= 0)
                return;

            // No attack: juce::ADSR starts at the peak already, so the hold
            // starts now. Otherwise it starts when the attack reaches 1.
            const auto& p = adsr.getParameters();
            if (p.attack <= 0.0f)
            {
                // With no decay either, juce::ADSR jumps straight to sustain;
                // holding 1.0 first would only add a step, so don't.
                if (p.decay > 0.0f)
                    holdLeft = holdSamples;
            }
            else
                waitingForPeak = true;
        }

        void noteOff() noexcept
        {
            holdLeft = 0;
            waitingForPeak = false;
            adsr.noteOff();   // releases from the level it is at (the peak while holding)
        }

        bool isActive() const noexcept { return adsr.isActive(); }

        float getNextSample() noexcept
        {
            if (holdLeft > 0)
            {
                --holdLeft;
                return 1.0f;   // the inner ADSR waits at its peak meanwhile
            }

            const float v = adsr.getNextSample();
            if (waitingForPeak && v >= 1.0f)
            {
                waitingForPeak = false;
                holdLeft = holdSamples;
            }
            return v;
        }

    private:
        juce::ADSR adsr;
        double sampleRate     = 44100.0;
        int    holdSamples    = 0;
        int    holdLeft       = 0;
        bool   waitingForPeak = false;
    };
}
