#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>
#include <vector>

namespace asampler
{
    // Offline time-stretching and formant shifting via a phase vocoder.
    //
    // Why offline rather than per-voice realtime:
    //
    //   A sampler's playback rate already *is* its pitch control, so the two
    //   are locked together — that is why plain resampling can never separate
    //   them. Breaking the lock needs spectral analysis and resynthesis, which
    //   is far too heavy to run per voice per block on the audio thread.
    //   Instead the pad renders a processed copy of its sample once, whenever
    //   the stretch/formant settings change, and voices play that copy at
    //   ratio 1.0. Renders happen on the loader thread, exactly like sample
    //   loading, so the audio thread never sees the cost.
    //
    // Quality notes — the three things that separate this from a naive
    // phase vocoder, all of which commercial stretchers do:
    //
    //   1. **Phase locking.** Propagating each bin's phase independently makes
    //      partials drift out of alignment, producing the smeared, chorused
    //      "phasiness" that gives cheap stretching away. We detect spectral
    //      peaks and rotate each peak's whole region of influence rigidly,
    //      preserving the vertical phase relationships inside a partial.
    //      (Laroche & Dolson, identity phase locking.)
    //
    //   2. **Transient preservation.** A phase vocoder smears attacks, because
    //      it is interpolating steady-state phase across the onset. At detected
    //      onsets we reset the synthesis phase to the analysis phase, which
    //      snaps the transient back into focus. This matters a lot for the
    //      drum and guitar material this plugin is aimed at.
    //
    //   3. **Cepstral formant handling.** The spectral envelope is extracted by
    //      low-quefrency liftering of the log spectrum, then warped and
    //      re-applied. Shifting the envelope independently of the harmonics is
    //      what makes a pitch move sound like the same instrument rather than
    //      a chipmunk.
    //
    //   4. **Stereo linking.** All channels share one phase rotation per bin,
    //      so the phase relationship between channels — the stereo image and
    //      the mono sum — survives the stretch. Processing channels on their
    //      own lets them drift apart and smears anything panned.
    struct StretchOptions
    {
        // Output duration multiplier. 2.0 = twice as long (half speed) with no
        // pitch change; 0.5 = half as long. Pitch is unaffected.
        double timeRatio = 1.0;

        // Spectral-envelope shift in semitones. Positive moves formants up
        // (smaller/brighter "body"), negative moves them down. 0 leaves the
        // envelope alone. Independent of pitch — this is the control that
        // resampling cannot give you.
        double formantSemitones = 0.0;

        // Reset synthesis phase at detected onsets so attacks stay sharp.
        // Worth leaving on for anything percussive; turning it off is
        // marginally smoother on sustained pads.
        bool preserveTransients = true;
    };

    // Is this options set a no-op? Callers use it to skip rendering entirely
    // rather than paying for an identity transform.
    bool isNeutral (const StretchOptions& options) noexcept;

    // Render `input` with the given options. Returns a new buffer with the
    // same channel count; length is approximately input length * timeRatio.
    //
    // Input shorter than one analysis window, or neutral options, returns a
    // copy. Never returns an empty buffer for non-empty input.
    //
    // `shouldAbort`, when given, is polled every few dozen frames; returning
    // true stops the render and returns an empty buffer. A render superseded
    // by a newer setting uses it to stop wasting a core.
    juce::AudioBuffer<float> processOffline (const juce::AudioBuffer<float>& input,
                                             double sampleRate,
                                             const StretchOptions& options,
                                             const std::function<bool()>& shouldAbort = {});

    // Spectral envelope of `magnitudes` by cepstral liftering, keeping the
    // first `liftOrder` cepstral coefficients. Exposed so the envelope
    // extraction can be tested on its own — it is the part of the formant
    // path most likely to be wrong in a way that is hard to hear.
    std::vector<float> computeSpectralEnvelope (const std::vector<float>& magnitudes,
                                                int liftOrder);
}
