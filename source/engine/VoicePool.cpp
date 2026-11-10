#include "VoicePool.h"
#include "Sample.h"

namespace asampler
{
    void VoicePool::prepare (double sr, int numVoices)
    {
        sampleRate = sr;
        slots.clear();
        slots.resize (static_cast<std::size_t> (juce::jmax (1, numVoices)));
        for (auto& slot : slots)
        {
            slot.voice.prepare (sr);
            slot.padIndex    = -1;
            slot.triggerNote = -1;
            slot.age         = 0;
        }
        ageCounter = 0;
    }

    void VoicePool::reset()
    {
        for (auto& slot : slots)
        {
            slot.voice.stop();
            slot.padIndex    = -1;
            slot.triggerNote = -1;
        }
        ageCounter = 0;
    }

    void VoicePool::triggerPad (int padIndex,
                                std::shared_ptr<const Sample> sample,
                                int triggerNote,
                                const VoiceConfig& cfg,
                                int maxVoicesOnPad)
    {
        if (sample == nullptr)
            return;

        // Per-pad cap pass: count current voices on this pad and remember the
        // oldest one in case we need to evict it. Skipped when the caller
        // passes 0 (no cap).
        int   countOnPad   = 0;
        Slot* oldestOnPad  = nullptr;
        if (maxVoicesOnPad > 0)
        {
            for (auto& slot : slots)
            {
                if (slot.padIndex == padIndex)
                {
                    ++countOnPad;
                    if (oldestOnPad == nullptr || slot.age < oldestOnPad->age)
                        oldestOnPad = &slot;
                }
            }
        }

        Slot* target = nullptr;

        if (maxVoicesOnPad > 0 && countOnPad >= maxVoicesOnPad && oldestOnPad != nullptr)
        {
            // Pad is at its cap — steal the oldest voice on this same pad
            // rather than borrowing a free pool slot. This is what makes
            // cap = 1 behave as true monophony.
            target = oldestOnPad;
        }
        else
        {
            for (auto& slot : slots)
            {
                if (slot.padIndex < 0)
                {
                    target = &slot;
                    break;
                }
            }

            if (target == nullptr)
            {
                // Pool is full — steal the oldest voice. Stable choice: lowest age.
                target = &slots.front();
                for (auto& slot : slots)
                    if (slot.age < target->age)
                        target = &slot;
            }
        }

        // Trigger == root at the engine layer: the plugin layer is responsible
        // for folding any "incoming note - root note" semitone delta into
        // cfg.pitchSemitones (so a Keyboard-mode pad still passes a single
        // effective pitch here). The stored triggerNote is the *incoming* MIDI
        // note that fired this voice, which is what per-note release matches.
        target->voice.start (std::move (sample), triggerNote, triggerNote, cfg);
        target->padIndex    = padIndex;
        target->triggerNote = triggerNote;
        target->age         = ++ageCounter;
    }

    void VoicePool::releasePad (int padIndex)
    {
        for (auto& slot : slots)
            if (slot.padIndex == padIndex)
                slot.voice.release();
    }

    void VoicePool::releasePadNote (int padIndex, int midiNote)
    {
        for (auto& slot : slots)
            if (slot.padIndex == padIndex && slot.triggerNote == midiNote)
                slot.voice.release();
    }

    void VoicePool::setPitchBendSemitones (float semitones)
    {
        for (auto& slot : slots)
            if (slot.padIndex >= 0)
                slot.voice.setPitchBendSemitones (semitones);
    }

    void VoicePool::setModWheel (float value)
    {
        for (auto& slot : slots)
            if (slot.padIndex >= 0)
                slot.voice.setModWheel (value);
    }

    void VoicePool::setPadGate (int padIndex, float gate)
    {
        for (auto& slot : slots)
            if (slot.padIndex == padIndex)
                slot.voice.setPadGate (gate);
    }

    void VoicePool::setPadLiveParams (int padIndex, const VoiceLiveParams& live)
    {
        for (auto& slot : slots)
            if (slot.padIndex == padIndex)
                slot.voice.setLiveParams (live);
    }

    bool VoicePool::hasActivePad (int padIndex) const noexcept
    {
        for (const auto& slot : slots)
            if (slot.padIndex == padIndex)
                return true;
        return false;
    }

    void VoicePool::chokePads (const int* padIndices, int count)
    {
        if (padIndices == nullptr || count <= 0)
            return;

        for (auto& slot : slots)
        {
            if (slot.padIndex < 0)
                continue;

            for (int i = 0; i < count; ++i)
            {
                if (slot.padIndex == padIndices[i])
                {
                    slot.voice.stop();
                    slot.padIndex = -1;
                    break;
                }
            }
        }
    }

    void VoicePool::renderAdding (juce::AudioBuffer<float>& output,
                                  int startSample,
                                  int numSamples)
    {
        for (auto& slot : slots)
        {
            if (slot.padIndex < 0)
                continue;

            slot.voice.renderAdding (output, startSample, numSamples);

            if (! slot.voice.isActive())
                slot.padIndex = -1;
        }
    }

    void VoicePool::renderAdding (juce::AudioBuffer<float>* const* destByPad,
                                  int startSample,
                                  int numSamples)
    {
        jassert (destByPad != nullptr);

        for (auto& slot : slots)
        {
            if (slot.padIndex < 0)
                continue;

            auto* dest = destByPad[slot.padIndex];
            jassert (dest != nullptr);
            slot.voice.renderAdding (*dest, startSample, numSamples);

            if (! slot.voice.isActive())
                slot.padIndex = -1;
        }
    }

    int VoicePool::countActive() const noexcept
    {
        int count = 0;
        for (const auto& slot : slots)
            if (slot.padIndex >= 0)
                ++count;
        return count;
    }
}
