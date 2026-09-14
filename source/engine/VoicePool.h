#pragma once

#include "Voice.h"

#include <vector>
#include <memory>
#include <cstdint>

namespace asampler
{
    class Sample;

    // Fixed-size pool of Voice instances with oldest-first voice stealing.
    //
    // The pool knows which pad each active voice belongs to so that pad-level
    // operations (release-on-note-off, choke groups) can find matching voices
    // in O(N). Render adds all active voices into the output buffer and frees
    // slots whose voices have deactivated.
    //
    // Not thread-safe. The plugin's processBlock owns the pool and is the
    // single caller for trigger / release / choke / render.
    class VoicePool
    {
    public:
        void prepare (double hostSampleRate, int numVoices);
        void reset();

        // Allocate a voice slot for `padIndex` (0-based) and start it on the
        // given sample. Uses trigger == root semantics so cfg.pitchSemitones
        // is the only source of pitch shift (MPC-style: one sample per pad,
        // root pitch).
        //
        // `maxVoicesOnPad` caps simultaneous voices for this pad: when the pad
        // already has >= cap voices sounding, the oldest same-pad voice is
        // stolen rather than allocating a fresh slot — so pad-level monophony
        // (cap = 1) works without choke-grouping the pad with itself.
        // Pass 0 to disable the per-pad cap (defer purely to pool-level
        // stealing). Otherwise, when no same-pad steal is needed, a free pool
        // slot is preferred; if the entire pool is full, the globally-oldest
        // voice is stolen.
        //
        // The voice retains its own shared_ptr to the sample, so it stays
        // playable even if the pad's sample is swapped immediately afterwards.
        void triggerPad (int padIndex,
                         std::shared_ptr<const Sample> sample,
                         int padNote,
                         const VoiceConfig& cfg,
                         int maxVoicesOnPad = 0);

        // Move all voices for `padIndex` into the ADSR release stage.
        // Used for Fixed-mode pads where note-off on the pad's assigned note
        // should release every voice on that pad.
        void releasePad (int padIndex);

        // Move voices for `padIndex` that were triggered by `midiNote` into
        // the ADSR release stage. Used for Keyboard-mode pads where a pad
        // can simultaneously hold voices for several incoming notes, and a
        // note-off should release only the matching voice(s).
        void releasePadNote (int padIndex, int midiNote);

        // Hard-stop every voice currently assigned to any pad in `padIndices`.
        // Used by choke groups.
        void chokePads (const int* padIndices, int count);

        // Push a new pitch bend offset (in semitones) onto every active voice.
        // Called from processBlock whenever a MIDI pitch-bend event arrives.
        void setPitchBendSemitones (float semitones);

        // Push aftertouch volume/cutoff modulation multipliers onto every
        // active voice belonging to `padIndex`. Both default to 1.0 (no
        // modulation). The plugin layer computes the per-pad destination
        // routing and amount; voices just apply the resulting multipliers.
        void setAftertouchModForPad (int padIndex,
                                     float volumeMultiplier,
                                     float cutoffMultiplier);

        // Push the mute/solo gate onto every active voice belonging to
        // `padIndex`. 1.0 = pad sounds, 0.0 = pad silenced. Voices keep
        // advancing their sample position when gated, so unmuting resumes
        // where the audio would have been (DAW-style track mute).
        void setPadGate (int padIndex, float gate);

        // Push live (per-block) parameter values onto every active voice on
        // `padIndex`. This is what makes automation and knob moves audible on
        // notes that are already sounding — see VoiceLiveParams.
        void setPadLiveParams (int padIndex, const VoiceLiveParams& live);

        // Is any voice currently allocated to `padIndex`? Used by the plugin
        // layer's legato-glide gate: only slide into the new note when the
        // pad is still sounding.
        bool hasActivePad (int padIndex) const noexcept;

        void renderAdding (juce::AudioBuffer<float>& output, int startSample, int numSamples);

        // Per-pad routing variant: each active voice renders into the buffer
        // pointed to by `destByPad[voice.padIndex]`. The caller guarantees that
        // every padIndex used by an active voice maps to a non-null destination
        // (the plugin layer routes to Main as a fallback for disabled per-pad
        // buses, so there is no "null destination" case at the engine layer).
        void renderAdding (juce::AudioBuffer<float>* const* destByPad,
                           int startSample, int numSamples);

        int countActive() const noexcept;

    private:
        struct Slot
        {
            Voice voice;
            int padIndex    = -1;  // 0-based, or -1 if free
            int triggerNote = -1;  // MIDI note that triggered this voice; for
                                   // Keyboard-mode pads this is the incoming
                                   // note, not the pad's root, so a per-note
                                   // release can match it.
            std::uint64_t age = 0; // monotonically increasing trigger order
        };

        std::vector<Slot> slots;
        std::uint64_t ageCounter = 0;
        double sampleRate = 44100.0;
    };
}
