#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "FilterMode.h"
#include "Lfo.h"
#include "LoopMode.h"
#include "Saturation.h"

#include <memory>

namespace asampler
{
    class Sample;

    struct VoiceConfig
    {
        float gainLin        = 1.0f;   // linear amplitude multiplier
        float pan            = 0.0f;   // -1 = full L, 0 = centre, +1 = full R (equal-power)
        float pitchSemitones = 0.0f;   // fine tune, applied on top of (triggerNote - rootNote)
        float pitchBendSemitones = 0.0f; // live MIDI pitch-bend offset (added on top of pitchSemitones)
        float startNorm      = 0.0f;   // playback start, 0..1 of sample length
        float endNorm        = 1.0f;   // playback end,   0..1 of sample length (must be > startNorm)
        bool  reverse        = false;  // play the [start..end] window backwards
        FilterMode filterMode = FilterMode::Off;
        float filterCutoffHz  = 20000.0f;
        float filterResonance = 0.707f;   // ~no resonance peak; 1/sqrt(2)
        LoopMode loopMode      = LoopMode::Off;
        float    loopStartNorm = 0.0f;    // 0..1 of source length
        float    loopEndNorm   = 1.0f;    // must be > loopStartNorm after clamp
        juce::ADSR::Parameters adsr { 0.005f, 0.100f, 1.0f, 0.100f };
        // Channel-aftertouch modulation, applied multiplicatively on top of
        // the base gain and filter cutoff. 1.0 = no modulation.
        float aftertouchVolumeMultiplier = 1.0f;
        float aftertouchCutoffMultiplier = 1.0f;
        // Mute/solo gate from the pad's per-pad bools, applied as an output
        // gain multiplier. 1.0 = pad sounds, 0.0 = pad silenced. Lives in
        // the config so newly triggered voices inherit the current gate, and
        // VoicePool can push live updates to active voices via setPadGate.
        float padGate = 1.0f;
        // Glide / portamento. The voice starts at a pitch offset of
        // `glideStartSemitones` (positive or negative) and slides toward 0
        // over `glideTimeMs` milliseconds. 0/0 disables glide. Plugin layer
        // fills these from the per-pad glide params and the pad's
        // last-triggered note.
        //
        // The slide is linear in *semitones*, not in frequency ratio. Lerping
        // the ratio (which is what a naive implementation does) makes the
        // pitch move at a non-constant musical rate — an octave slide lingers
        // at one end and rushes the other. Barely audible over 2-3 semitones,
        // clearly wrong over the octave jumps 808s routinely make.
        float glideStartSemitones = 0.0f;
        float glideTimeMs         = 0.0f;

        // Pitch envelope. The voice's pitch starts `pitchEnvSemitones` above
        // (or below) its target and decays back to the target over
        // `pitchEnvTimeMs`. 0 amount disables it. Classic 808 / kick "punch".
        float pitchEnvSemitones = 0.0f;
        float pitchEnvTimeMs    = 0.0f;

        // Per-pad LFO. `lfoDest == Off` (or depth 0) is a bypass. Depth is
        // 0..1; the per-destination amount (how far ±1 of the LFO pushes the
        // target) is baked into the voice's render.
        LfoShape lfoShape = LfoShape::Sine;
        LfoDest  lfoDest  = LfoDest::Off;
        float    lfoRateHz = 1.0f;
        float    lfoDepth  = 0.0f;

        // Saturation / bit-crush "character" chain, applied post-filter.
        SaturationMode satMode        = SaturationMode::Tube;
        float          satAmount      = 0.0f;   // 0 = bypass
        int            bitDepth       = 16;     // 16 = bypass
        int            downsampleFactor = 1;    // 1 = bypass (sample-rate reduction)

        // Click-free edges. Fade-in / fade-out are applied at the two window
        // boundaries; loop crossfade blends the loop tail into its head for
        // Forward loops. All in milliseconds; 0 = off. `snapToZeroCrossings`
        // nudges the start/end (and loop) indices onto the nearest zero
        // crossing at trigger time.
        float fadeInMs     = 0.0f;
        float fadeOutMs    = 0.0f;
        float loopXfadeMs  = 0.0f;
        bool  snapToZeroCrossings = false;

        // Filter envelope. A dedicated ADSR on the cutoff, with `filterEnvOctaves`
        // setting how far (and in which direction) a full envelope pushes it.
        // 0 octaves is a bypass. This is the per-hit filter sweep that LFO,
        // velocity and aftertouch modulation could not express.
        float filterEnvOctaves = 0.0f;
        juce::ADSR::Parameters filterEnvAdsr { 0.001f, 0.200f, 0.0f, 0.100f };

        // Stereo shaping, applied last. `stereoWidth` scales the M/S side
        // signal: 0 = mono, 1 = unchanged, 2 = double width. `monoBelowHz`
        // collapses everything under that frequency to mono (0 = off) — the
        // standard way to keep bass and 808s centred and phase-coherent.
        float stereoWidth = 1.0f;
        float monoBelowHz = 0.0f;

        // Per-hit velocity modulation factors, kept separate from the values
        // they have already been folded into (gainLin / filterCutoffHz). The
        // voice needs them so that a *live* parameter push from the plugin
        // layer — which only knows the pad's base value — can re-apply this
        // voice's own velocity offset instead of erasing it.
        float hitGainScale        = 1.0f;   // velocity-curve output, already in gainLin
        float hitCutoffMultiplier = 1.0f;   // vel->cutoff factor, already in filterCutoffHz
    };

    // The subset of VoiceConfig that can meaningfully change while a voice is
    // sounding. The plugin layer pushes these every block so automation and
    // knob moves affect notes that are already ringing (a filter sweep over a
    // held 808, a volume ride on a long chop). Everything else — loop points,
    // ADSR shape, sample window — is structural and stays trigger-time only.
    //
    // Values are the *pad's* base values; the voice re-applies its own
    // per-hit velocity modulation on top.
    struct VoiceLiveParams
    {
        float gainLin         = 1.0f;
        float pan             = 0.0f;
        float filterCutoffHz  = 20000.0f;
        float filterResonance = 0.707f;
        float satAmount       = 0.0f;
        float lfoRateHz       = 1.0f;
        float lfoDepth        = 0.0f;
        float stereoWidth     = 1.0f;
        float monoBelowHz     = 0.0f;
        float filterEnvOctaves = 0.0f;
    };

    // One playing voice. Holds a shared_ptr to its source Sample so that the
    // owning pad can swap to a new sample at any time without yanking memory
    // out from under voices that are still rendering the old one.
    //
    // Not thread-safe. The plugin layer is responsible for sequencing start /
    // release / render calls on the audio thread.
    class Voice
    {
    public:
        void prepare (double hostSampleRate);

        // Begin playback. The voice retains a strong reference to `source` for
        // the duration of playback — the caller can swap the pad's sample
        // immediately afterwards without affecting this voice.
        void start (std::shared_ptr<const Sample> source,
                    int triggerNote,
                    int rootNote,
                    const VoiceConfig& cfg);

        void release() noexcept;
        void stop() noexcept;

        bool isActive() const noexcept { return active; }

        // Live pitch-bend update (the global "pitch bend in semitones" the
        // processor maintains from MIDI 0xE0 messages). Re-derives pitchRatio
        // from the immutable base + this offset + the current direction so
        // ping-pong's mid-flight direction flips are preserved.
        void setPitchBendSemitones (float semitones) noexcept;

        // Live channel-aftertouch modulation. Multiplies the voice's base
        // gain and (if filter is on) base filter cutoff. 1.0 = bypass.
        void setAftertouchModulation (float volumeMultiplier,
                                      float cutoffMultiplier) noexcept;

        // Live mute/solo gate update from the plugin layer. 1.0 = sounds,
        // 0.0 = silenced. Cheap atomic-ish flip; the voice keeps advancing
        // its sample position so unmuting resumes where the user would
        // expect (DAW-style track mute).
        void setPadGate (float gate) noexcept { padGate = gate; }

        // Live parameter push from the plugin layer, called once per block for
        // every sounding voice. Gain / pan / cutoff ramp to their new values
        // over kSmoothingSeconds so automation doesn't produce zipper noise;
        // the rest take effect immediately (they are either already smooth by
        // nature or not audible as steps).
        void setLiveParams (const VoiceLiveParams& live) noexcept;

        void renderAdding (juce::AudioBuffer<float>& output, int startSample, int numSamples);

    private:
        void updatePitchRatio() noexcept;
        void updateFilterCutoff() noexcept;

        std::shared_ptr<const Sample> source;
        double hostRate     = 44100.0;
        double readPosition = 0.0;
        double basePitchRatio = 1.0;   // positive magnitude derived from note + sample-rate + cfg.pitchSemitones
        float  pitchBendSemitones = 0.0f;
        double pitchBendScale = 1.0;   // cached 2^(pitchBendSemitones/12); recomputed only on bend change
        // Glide is tracked in semitones and converted to a ratio per sample, so
        // the slide moves at a constant musical rate. `glideSemitones` walks
        // linearly toward 0.
        double glideSemitones        = 0.0;
        double glideSemitoneStep     = 0.0;
        double glideScale            = 1.0;   // 2^(glideSemitones/12), recomputed as it moves
        int    glideRemainingSamples = 0;
        double pitchEnvScale     = 1.0;  // decaying pitch-env multiplier; lerps toward 1.0
        double pitchEnvIncrement = 0.0;  // per-sample step applied to pitchEnvScale
        int    pitchEnvRemainingSamples = 0;
        double direction    = 1.0;     // +1 forward, -1 reverse / ping-pong reflection
        double pitchRatio   = 1.0;     // = basePitchRatio * pitchBendScale * glideScale * pitchEnvScale * direction
        int    startIdx     = 0;       // inclusive lower bound of [startIdx, endIdx)
        int    endIdx       = 0;       // exclusive upper bound; renderer needs idx+1 in range
        float  gainL = 1.0f;
        float  gainR = 1.0f;
        float  baseGainLin = 1.0f;     // current (post-smoothing) gain, for per-sample LFO volume/pan recompute
        float  basePan     = 0.0f;     // current (post-smoothing) pan,  for per-sample LFO pan recompute
        float  aftertouchGainMultiplier   = 1.0f;
        float  aftertouchCutoffMultiplier = 1.0f;
        float  baseCutoffHz               = 20000.0f;  // current (post-smoothing) cutoff
        float  padGate                    = 1.0f;

        // Ramps for the three parameters where a step change is audible as a
        // click or zipper. Everything else in VoiceLiveParams is applied
        // directly. Reset to the trigger-time value in start() so a fresh
        // voice never ramps up from the previous voice's setting.
        static constexpr double kSmoothingSeconds = 0.02;   // 20 ms
        juce::SmoothedValue<float> smoothedGain;
        juce::SmoothedValue<float> smoothedPan;
        juce::SmoothedValue<float> smoothedCutoff;

        // Per-hit velocity offsets, re-applied on top of live pad values so a
        // live push doesn't flatten this voice's velocity response.
        float hitGainScale        = 1.0f;
        float hitCutoffMultiplier = 1.0f;

        // Change detection so the per-sample gain/pan and filter-coefficient
        // recomputes (trig + coefficient math) only run when a value actually
        // moved. Initialised out of range to force the first computation.
        float lastSmoothedGain = -1.0f;
        float lastSmoothedPan  = -2.0f;
        float lastCutoffSet    = -1.0f;
        float currentResonance = 0.707f;
        juce::ADSR envelope;

        // Second envelope, dedicated to the filter cutoff. Runs in parallel
        // with the amp envelope and is released at the same time.
        juce::ADSR filterEnv;
        float      filterEnvOctaves = 0.0f;
        bool       filterEnvActive  = false;

        // Stereo shaping state. The mono-below crossover uses a one-pole
        // low-pass per channel; the low band is summed to mono and the
        // (unfiltered minus low) high band keeps its stereo image.
        float stereoWidth     = 1.0f;
        float monoBelowHz     = 0.0f;
        bool  stereoShaping   = false;
        float monoLpCoeff     = 0.0f;   // one-pole coefficient for monoBelowHz
        float monoLpStateL    = 0.0f;
        float monoLpStateR    = 0.0f;

        juce::dsp::StateVariableTPTFilter<float> filter;
        FilterMode filterMode = FilterMode::Off;
        LoopMode   loopMode   = LoopMode::Off;
        int        loopStartIdx = 0;     // inclusive
        int        loopEndIdx   = 0;     // exclusive

        // Per-pad LFO state. `lfoActive` gates the whole modulation path so the
        // common (no-LFO) case pays only one bool test per sample.
        bool     lfoActive  = false;
        LfoShape lfoShape    = LfoShape::Sine;
        LfoDest  lfoDest      = LfoDest::Off;
        double   lfoPhase     = 0.0;   // [0, 1)
        double   lfoPhaseInc  = 0.0;   // per-sample phase advance
        float    lfoDepth     = 0.0f;

        // Saturation / bit-crush. `characterActive` gates the whole chain.
        bool           characterActive = false;
        SaturationMode satMode         = SaturationMode::Tube;
        float          satAmount       = 0.0f;
        int            bitDepth        = 16;
        int            downsampleFactor = 1;
        int            crushCounter    = 0;      // downsample hold countdown
        float          crushHoldL      = 0.0f;
        float          crushHoldR      = 0.0f;

        // Click-free edges. Fade lengths in samples; loop crossfade in samples.
        int   fadeInSamples  = 0;
        int   fadeOutSamples = 0;
        int   loopXfadeSamples = 0;

        bool   active = false;
    };
}
