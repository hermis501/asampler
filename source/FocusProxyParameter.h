#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

namespace asampler
{
    // A host parameter that forwards to the same-named parameter on whichever
    // pad is currently focused.
    //
    // Why this exists (accessibility):
    //
    //   Exposing every per-pad control as a host parameter is what makes the
    //   plugin readable by a screen reader at all — REAPER's FX parameter list
    //   speaks host parameters with no custom UI work. But that design
    //   multiplies: ~90 controls x 32 pads is ~2,800 parameters, and a blind
    //   user hunting for "Pad 9 Filter Cutoff" has to arrow through hundreds of
    //   entries to reach it.
    //
    //   The Focus group collapses that. One "Focused Pad" parameter selects the
    //   pad; a single mirrored group of ~90 "Focus ..." parameters reads and
    //   writes through to it. The user navigates ~90 parameters instead of 2,800
    //   and changes pad with a single parameter move. The per-pad parameters
    //   stay exposed, so automation and host-side control lose nothing.
    //
    // All 32 targets are cached at setup so no string lookup happens on any
    // hot path. Range, step count, label and text formatting are taken from
    // the reference target (pad 1) — every pad's copy of a given parameter is
    // built identically, so any of them describes the shape correctly.
    class FocusProxyParameter final : public juce::AudioProcessorParameterWithID
    {
    public:
        // `id` must be stable ("focus_cutoff"): VST3 derives the host's
        // parameter ID from it, so saved automation survives regrouping.
        FocusProxyParameter (const juce::String& id, juce::String parameterName, int numPads)
            : juce::AudioProcessorParameterWithID (juce::ParameterID { id, 1 }, parameterName),
              displayName (std::move (parameterName)), targets ((std::size_t) numPads, nullptr)
        {
        }


        // Two-phase init: the targets don't exist until the APVTS has been
        // constructed, which happens after the parameter layout (including
        // this proxy) is built. The processor calls this from
        // cacheParamPointers().
        void setTargets (std::vector<juce::RangedAudioParameter*> t,
                         const std::atomic<float>* focusedPadValue) noexcept
        {
            targets     = std::move (t);
            focusedPad  = focusedPadValue;
        }

        // Tell the host this proxy now reads a different underlying value.
        // Called for every proxy when the Focused Pad parameter moves, so the
        // FX list (and the screen reader) refresh instead of showing stale
        // values for the previously focused pad.
        void refreshFromFocus()
        {
            if (auto* t = target())
                sendValueChangedMessageToListeners (t->getValue());
        }

        // ---- AudioProcessorParameter ----

        float getValue() const override
        {
            auto* t = target();
            return t != nullptr ? t->getValue() : 0.0f;
        }

        void setValue (float newValue) override
        {
            if (auto* t = target())
                t->setValueNotifyingHost (newValue);
        }

        float getDefaultValue() const override
        {
            auto* r = reference();
            return r != nullptr ? r->getDefaultValue() : 0.0f;
        }

        juce::String getName (int maximumStringLength) const override
        {
            return displayName.substring (0, maximumStringLength);
        }

        juce::String getLabel() const override
        {
            auto* r = reference();
            return r != nullptr ? r->getLabel() : juce::String();
        }

        int getNumSteps() const override
        {
            auto* r = reference();
            return r != nullptr ? r->getNumSteps()
                                : juce::AudioProcessorParameter::getNumSteps();
        }

        bool isDiscrete() const override
        {
            auto* r = reference();
            return r != nullptr && r->isDiscrete();
        }

        bool isBoolean() const override
        {
            auto* r = reference();
            return r != nullptr && r->isBoolean();
        }

        // Screen readers speak this string, so it must come from the target's
        // own formatter ("-6.00 dB", not "0.75"). The *focused* pad's, not the
        // reference pad's: slice points read as time in that pad's sample.
        juce::String getText (float normalisedValue, int maximumStringLength) const override
        {
            auto* t = target();
            return t != nullptr ? t->getText (normalisedValue, maximumStringLength)
                                : juce::String (normalisedValue);
        }

        float getValueForText (const juce::String& text) const override
        {
            auto* t = target();
            return t != nullptr ? t->getValueForText (text) : 0.0f;
        }

        bool isAutomatable() const override   { return true; }

        // A change here changes what other parameters report, which is exactly
        // what a meta parameter is. Hosts use this to avoid treating the value
        // as ordinary automation data.
        bool isMetaParameter() const override { return false; }

    private:
        // Currently focused pad, clamped into range. Returns 0 when the focus
        // parameter hasn't been wired yet (during construction).
        int focusedIndex() const noexcept
        {
            if (focusedPad == nullptr || targets.empty())
                return 0;

            // The Focused Pad parameter is 1-based ("Pad 1".."Pad 32").
            const int oneBased = juce::roundToInt (focusedPad->load());
            return juce::jlimit (0, static_cast<int> (targets.size()) - 1, oneBased - 1);
        }

        juce::RangedAudioParameter* target() const noexcept
        {
            if (targets.empty())
                return nullptr;
            return targets[(std::size_t) focusedIndex()];
        }

        // Used for anything describing the parameter's *shape* rather than its
        // value. Every pad's copy is identical, so pad 1 is a safe stand-in and
        // stays valid even if the focus index is momentarily out of range.
        juce::RangedAudioParameter* reference() const noexcept
        {
            return targets.empty() ? nullptr : targets[0];
        }

        juce::String                             displayName;
        std::vector<juce::RangedAudioParameter*> targets;
        const std::atomic<float>*                focusedPad = nullptr;
    };
}
