#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "FocusProxyParameter.h"

#include "engine/AtomicSharedPtr.h"
#include "engine/FilterMode.h"
#include "engine/LoopMode.h"
#include "engine/Sample.h"
#include "engine/VelocityCurve.h"
#include "engine/PhaseVocoder.h"
#include "engine/VoicePool.h"
#include "ReaperIntegration.h"

#include <array>
#include <atomic>
#include <memory>

namespace asampler
{
    // Two MPC-style banks of 16: bank A is pads 1-16, bank B pads 17-32.
    inline constexpr int kNumPads   = 32;
    inline constexpr int kPadsPerBank = 16;
    // Every pad at the per-pad cap of 8, so no pad at its cap can starve
    // another: rolls, long releases and stacked layers all fit at once. Idle
    // voices cost nothing; the pool only matters when they all sound.
    inline constexpr int kNumVoices = kNumPads * 8;
    // Sample layers per pad: velocity layers, round-robin variations, release
    // samples or key zones. Eight is enough for a real multisampled instrument
    // across a few octaves, or four velocities with a release sample each.
    inline constexpr int kNumLayers = 8;

    // Widest interval either side of a layer's root for which a formant-
    // corrected variant will be pre-rendered. Two octaves covers any realistic
    // playable range; the cap exists because each variant is a full copy of the
    // sample, so an uncapped range is an unbounded memory commitment.
    inline constexpr int kMaxFormantOffset   = 24;
    inline constexpr int kFormantVariantSlots = kMaxFormantOffset * 2 + 1;   // 49

    // What a layer's formant table was built for. Comparing this against the
    // current settings is how the render timer decides whether to rebuild.
    struct FormantTableKey
    {
        bool   enabled   = false;
        double timeRatio = 1.0;
        double formant   = 0.0;
        int    minOffset = 0;
        int    maxOffset = 0;

        // What each variant corrects. correctFormants: shift formants by -k
        // (Formant Correct). keepLength: stretch by 2^(k/12) so the variant,
        // played k semitones up, lasts as long as the original (Keep Length).
        bool   correctFormants = true;
        bool   keepLength      = false;

        bool operator== (const FormantTableKey& o) const noexcept
        {
            return enabled == o.enabled
                && correctFormants == o.correctFormants
                && keepLength == o.keepLength
                && minOffset == o.minOffset
                && maxOffset == o.maxOffset
                && std::abs (timeRatio - o.timeRatio) < 1.0e-6
                && std::abs (formant   - o.formant)   < 1.0e-6;
        }

        bool operator!= (const FormantTableKey& o) const noexcept { return ! (*this == o); }
    };

    // Formant-corrected renders of one layer, indexed by *total* playback
    // offset in semitones (offset + kMaxFormantOffset). Null entries mean
    // "not rendered"; the audio thread falls back to the uncorrected sample.
    //
    // Why a pre-rendered table rather than realtime correction:
    //
    //   Playing a sample at ratio r shifts its formants by r, and undoing that
    //   needs spectral resynthesis — impossible per voice per block. But the
    //   correction for a voice playing k semitones from the root is always the
    //   *same* thing: a formant shift of -k. So the corrections can all be
    //   rendered up front and looked up with an array index at note-on.
    //
    //   The index is the voice's whole pitch offset — note interval, the Pitch
    //   parameter, tempo-sync repitch and Vel->Pitch together — rounded to the
    //   nearest semitone. Rounding is what makes the continuous contributions
    //   tractable: half a semitone of residual formant error is inaudible,
    //   while pre-rendering every 0.01-semitone Pitch value is not possible.
    struct FormantTable
    {
        std::array<std::shared_ptr<const Sample>, kFormantVariantSlots> variants;
    };

    // Auto-chop algorithm. Append-only: the value index is serialised by
    // AudioParameterChoice, so reordering would corrupt saved state.
    enum class ChopMode : int
    {
        Equal              = 0,   // pad i gets [i/N, (i+1)/N] of the source
        Transients         = 1,   // log-energy onsets — good for percussive
        TransientsSpectral = 2    // spectral-flux onsets — handles melodic / soft attacks
    };

    // How a pad picks among its loaded velocity layers on note-on.
    // Append-only: AudioParameterChoice serialises by index.
    enum class LayerMode : int
    {
        Velocity   = 0,   // band-by-velocity (default)
        RoundRobin = 1,   // cycle through loaded layers in index order
        Random     = 2,   // uniformly random loaded layer

        // Key zones: each layer covers its own note range and declares its own
        // root, so one pad can hold a multisampled instrument instead of one
        // sample stretched across the whole keyboard. Without this, a bass or
        // guitar sample is chipmunked at the top of its range and muddy at the
        // bottom. Only meaningful for pads in Keyboard mode.
        KeyZones   = 3
    };

    // Glide / portamento behaviour per pad. Append-only:
    // AudioParameterChoice serialises by index.
    enum class GlideMode : int
    {
        Off    = 0,   // every retrigger jumps to its own pitch immediately
        Legato = 1,   // glide only if a previous voice on the same pad is still sounding
        Always = 2,   // glide from the pad's most recent trigger note, even after it has decayed

        // Rate-based variants: Glide Time is read as milliseconds *per
        // semitone* rather than a total duration, so a one-semitone slide
        // stays snappy while an octave jump takes proportionally longer. This
        // is how hardware portamento usually behaves, and it means one setting
        // works across the whole range of intervals.
        LegatoRate = 3,
        AlwaysRate = 4
    };

    // Is this glide mode rate-based (ms per semitone) rather than fixed-time?
    constexpr bool isRateGlide (GlideMode m) noexcept
    {
        return m == GlideMode::LegatoRate || m == GlideMode::AlwaysRate;
    }

    // Does this glide mode require a still-sounding voice on the pad?
    constexpr bool isLegatoGlide (GlideMode m) noexcept
    {
        return m == GlideMode::Legato || m == GlideMode::LegatoRate;
    }

    // How a pad reconciles its sample with the project tempo. Append-only:
    // AudioParameterChoice serialises by index.
    enum class StretchMode : int
    {
        // Speed the sample up or down to fit the tempo, letting pitch move with
        // it. The classic MPC / turntable behaviour, and often the wanted
        // sound in hip-hop — a sped-up soul loop is a genre staple.
        Repitch = 0,

        // Time-stretch to fit the tempo with pitch held constant. What you
        // need to drop a 90 BPM guitar riff into an 80 BPM beat without
        // detuning it. Costs an offline render (see engine/PhaseVocoder.h).
        Stretch = 1
    };

    // Musical scale for the key/scale lock. Append-only: AudioParameterChoice
    // serialises by index. Values are semitone offsets from the key root.
    enum class Scale : int
    {
        Chromatic       = 0,
        Major           = 1,
        NaturalMinor    = 2,
        HarmonicMinor   = 3,
        PentatonicMinor = 4,
        PentatonicMajor = 5,
        Blues           = 6,
        Dorian          = 7,
        Phrygian        = 8,
        Mixolydian      = 9
    };

    // How a pad listens for incoming MIDI notes. Append-only: AudioParameterChoice
    // serialises by index.
    //
    // Fixed (default, the MPC model): pad fires only when the incoming MIDI
    // note matches its single assigned `Note` param. Pitch comes purely from
    // the Pitch param.
    //
    // Keyboard (ReaSamplOmatic-style): pad fires for any incoming note in
    // [Low Note, High Note]. The assigned `Note` becomes the *root*: the
    // sample plays at +N semitones where N = (incomingNote - root). The
    // Pitch param still adds on top as a fine-tune. This lets one pad (e.g.
    // an 808) cover a wide stretch of the keyboard while other pads stay
    // on fixed drum notes in the same kit.
    enum class PadMode : int
    {
        Fixed    = 0,
        Keyboard = 1
    };

    // Note-repeat division. Append-only: AudioParameterChoice serialises by
    // index, so reordering would silently corrupt saved state.
    enum class NoteRepeatRate : int
    {
        Quarter             = 0,   // 1 trigger / beat
        Eighth              = 1,   // 2
        EighthTriplet       = 2,   // 3
        Sixteenth           = 3,   // 4 (default — trap hat repeat)
        SixteenthTriplet    = 4,   // 6
        ThirtySecond        = 5,   // 8
        ThirtySecondTriplet = 6    // 12
    };

    // Semitone degrees of `scale`, relative to its root. Chromatic returns all
    // twelve. Header-inline so tests can exercise it without linking the
    // plugin target.
    inline juce::Array<int> scaleDegrees (Scale scale)
    {
        switch (scale)
        {
            case Scale::Major:           return { 0, 2, 4, 5, 7, 9, 11 };
            case Scale::NaturalMinor:    return { 0, 2, 3, 5, 7, 8, 10 };
            case Scale::HarmonicMinor:   return { 0, 2, 3, 5, 7, 8, 11 };
            case Scale::PentatonicMinor: return { 0, 3, 5, 7, 10 };
            case Scale::PentatonicMajor: return { 0, 2, 4, 7, 9 };
            case Scale::Blues:           return { 0, 3, 5, 6, 7, 10 };
            case Scale::Dorian:          return { 0, 2, 3, 5, 7, 9, 10 };
            case Scale::Phrygian:        return { 0, 1, 3, 5, 7, 8, 10 };
            case Scale::Mixolydian:      return { 0, 2, 4, 5, 7, 9, 10 };
            case Scale::Chromatic:
            default:                     return { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        }
    }

    // Snap `midiNote` to the nearest note of `scale` rooted on `keyRoot`
    // (0 = C .. 11 = B). Ties round down, so the result is deterministic.
    // Chromatic is a no-op.
    inline int snapNoteToScale (int midiNote, int keyRoot, Scale scale)
    {
        if (scale == Scale::Chromatic)
            return midiNote;

        const auto degrees = scaleDegrees (scale);
        if (degrees.isEmpty())
            return midiNote;

        // Distance above the key root, folded into one octave.
        const int rel = ((midiNote - keyRoot) % 12 + 12) % 12;

        // Consider each degree in the octave below, at, and above `rel`. A note
        // just under the octave must be able to snap *up* to the next root
        // rather than down the length of the scale — comparing only within one
        // octave would send B up-a-semitone-from-C down eleven semitones.
        int bestOffset = degrees[0];
        int bestDist   = 1000;
        for (const int d : degrees)
        {
            for (const int candidate : { d - 12, d, d + 12 })
            {
                const int dist = std::abs (rel - candidate);
                if (dist < bestDist)
                {
                    bestDist   = dist;
                    bestOffset = candidate;
                }
            }
        }

        return midiNote - rel + bestOffset;
    }

    namespace param_ids
    {
        inline constexpr auto masterGain     = "master_gain";
        inline constexpr auto chopCount      = "chop_count";
        inline constexpr auto chopMode       = "chop_mode";
        inline constexpr auto editLayer      = "edit_layer";
        inline constexpr auto midiLearnArm   = "midi_learn_arm";
        inline constexpr auto pitchBendRange = "pitch_bend_range";
        inline constexpr auto auditionOnEdit = "audition_on_edit";

        // Velocity of pads played from the computer keyboard (1..127).
        inline constexpr auto keyVelocity    = "key_velocity";
        // Move a freshly loaded sample's Start past its leading silence.
        inline constexpr auto trimOnLoad     = "trim_on_load";

        // The pad the "Focus" proxy parameter group currently points at.
        // See FocusProxyParameter — this is the accessibility keystone: it
        // turns ~2,800 host parameters into ~90 the user actually navigates.
        inline constexpr auto focusedPad = "focused_pad";

        // Global musical key + scale, used by any pad with Scale Lock on.
        inline constexpr auto scaleKey  = "scale_key";
        inline constexpr auto scaleType = "scale_type";

        inline juce::String padVolume     (int pad) { return "pad_" + juce::String (pad) + "_volume"; }
        inline juce::String padPan        (int pad) { return "pad_" + juce::String (pad) + "_pan"; }
        inline juce::String padPitch      (int pad) { return "pad_" + juce::String (pad) + "_pitch"; }
        inline juce::String padNote       (int pad) { return "pad_" + juce::String (pad) + "_note"; }
        inline juce::String padAttack     (int pad) { return "pad_" + juce::String (pad) + "_attack"; }
        inline juce::String padDecay      (int pad) { return "pad_" + juce::String (pad) + "_decay"; }
        inline juce::String padSustain    (int pad) { return "pad_" + juce::String (pad) + "_sustain"; }
        inline juce::String padRelease    (int pad) { return "pad_" + juce::String (pad) + "_release"; }
        inline juce::String padChokeGroup (int pad) { return "pad_" + juce::String (pad) + "_choke"; }
        inline juce::String padStart         (int pad) { return "pad_" + juce::String (pad) + "_start"; }
        inline juce::String padEnd           (int pad) { return "pad_" + juce::String (pad) + "_end"; }
        inline juce::String padReverse       (int pad) { return "pad_" + juce::String (pad) + "_reverse"; }
        inline juce::String padVelocityCurve (int pad) { return "pad_" + juce::String (pad) + "_velcurve"; }
        inline juce::String padFilterMode      (int pad) { return "pad_" + juce::String (pad) + "_filtmode"; }
        inline juce::String padFilterCutoff    (int pad) { return "pad_" + juce::String (pad) + "_filtcut";  }
        inline juce::String padFilterResonance (int pad) { return "pad_" + juce::String (pad) + "_filtres";  }
        inline juce::String padLoopMode  (int pad) { return "pad_" + juce::String (pad) + "_loopmode";  }
        inline juce::String padLoopStart (int pad) { return "pad_" + juce::String (pad) + "_loopstart"; }
        inline juce::String padLoopEnd   (int pad) { return "pad_" + juce::String (pad) + "_loopend";   }
        inline juce::String padLayerMode (int pad) { return "pad_" + juce::String (pad) + "_layermode"; }
        inline juce::String padRepeatEnabled (int pad) { return "pad_" + juce::String (pad) + "_rptenable"; }
        inline juce::String padRepeatRate    (int pad) { return "pad_" + juce::String (pad) + "_rptrate";   }
        inline juce::String padOneShot       (int pad) { return "pad_" + juce::String (pad) + "_oneshot";   }
        inline juce::String padMute (int pad) { return "pad_" + juce::String (pad) + "_mute"; }
        inline juce::String padSolo (int pad) { return "pad_" + juce::String (pad) + "_solo"; }
        inline juce::String padGlideMode (int pad) { return "pad_" + juce::String (pad) + "_glidemode"; }
        inline juce::String padGlideTime (int pad) { return "pad_" + juce::String (pad) + "_glidetime"; }
        inline juce::String padMaxVoices (int pad) { return "pad_" + juce::String (pad) + "_maxvoices"; }
        inline juce::String padMode      (int pad) { return "pad_" + juce::String (pad) + "_mode";      }
        inline juce::String padLowNote   (int pad) { return "pad_" + juce::String (pad) + "_lownote";   }
        inline juce::String padHighNote  (int pad) { return "pad_" + juce::String (pad) + "_highnote";  }

        // Saturation / bit-crush character chain.
        inline juce::String padSatMode    (int pad) { return "pad_" + juce::String (pad) + "_satmode";    }
        inline juce::String padSatAmount  (int pad) { return "pad_" + juce::String (pad) + "_satamt";     }
        inline juce::String padBitDepth   (int pad) { return "pad_" + juce::String (pad) + "_bits";       }
        inline juce::String padDownsample (int pad) { return "pad_" + juce::String (pad) + "_downsample"; }

        // Pitch envelope.
        inline juce::String padPitchEnvAmount (int pad) { return "pad_" + juce::String (pad) + "_penvamt";  }
        inline juce::String padPitchEnvTime   (int pad) { return "pad_" + juce::String (pad) + "_penvtime"; }

        // Slice cleanup: edge fades, loop crossfade, zero-crossing snap.
        inline juce::String padFadeIn    (int pad) { return "pad_" + juce::String (pad) + "_fadein";   }
        inline juce::String padFadeOut   (int pad) { return "pad_" + juce::String (pad) + "_fadeout";  }
        inline juce::String padLoopXfade (int pad) { return "pad_" + juce::String (pad) + "_loopxf";   }
        inline juce::String padZeroSnap  (int pad) { return "pad_" + juce::String (pad) + "_zerosnap"; }

        // Per-pad LFO.
        inline juce::String padLfoRate  (int pad) { return "pad_" + juce::String (pad) + "_lforate";  }
        inline juce::String padLfoDepth (int pad) { return "pad_" + juce::String (pad) + "_lfodepth"; }
        inline juce::String padLfoShape (int pad) { return "pad_" + juce::String (pad) + "_lfoshape"; }
        inline juce::String padLfoDest  (int pad) { return "pad_" + juce::String (pad) + "_lfodest";  }

        // Velocity modulation.
        inline juce::String padVelToCutoff (int pad) { return "pad_" + juce::String (pad) + "_v2cut";   }
        inline juce::String padVelToPitch  (int pad) { return "pad_" + juce::String (pad) + "_v2pitch"; }
        inline juce::String padVelToStart  (int pad) { return "pad_" + juce::String (pad) + "_v2start"; }

        // Note-repeat feel.
        inline juce::String padSwing    (int pad) { return "pad_" + juce::String (pad) + "_swing"; }
        inline juce::String padHumanize (int pad) { return "pad_" + juce::String (pad) + "_human"; }

        // Tempo sync. When `padTempoSync` is on, the pad's playback ratio is
        // scaled by (hostBpm / padOriginalBpm) so a chopped loop stays in time
        // with the project. Pitch moves with tempo — the classic MPC / repitch
        // behaviour, which is usually what hip-hop wants.
        inline juce::String padTempoSync   (int pad) { return "pad_" + juce::String (pad) + "_tsync"; }
        inline juce::String padOriginalBpm (int pad) { return "pad_" + juce::String (pad) + "_obpm";  }

        // Per-layer gain trim, so velocity layers recorded at different levels
        // can be matched without editing the files. `layer` is 1-based.
        inline juce::String padLayerGain (int pad, int layer)
        {
            return "pad_" + juce::String (pad) + "_laygain" + juce::String (layer);
        }

        // Per-layer key mapping, used by LayerMode::KeyZones. Each layer covers
        // [low, high] and plays unshifted at its own root.
        inline juce::String padLayerLowNote (int pad, int layer)
        {
            return "pad_" + juce::String (pad) + "_laylo" + juce::String (layer);
        }
        inline juce::String padLayerHighNote (int pad, int layer)
        {
            return "pad_" + juce::String (pad) + "_layhi" + juce::String (layer);
        }
        inline juce::String padLayerRootNote (int pad, int layer)
        {
            return "pad_" + juce::String (pad) + "_layroot" + juce::String (layer);
        }

        // Filter envelope.
        inline juce::String padFiltEnvAmount  (int pad) { return "pad_" + juce::String (pad) + "_fenvamt";  }
        inline juce::String padFiltEnvAttack  (int pad) { return "pad_" + juce::String (pad) + "_fenvatk";  }
        inline juce::String padFiltEnvDecay   (int pad) { return "pad_" + juce::String (pad) + "_fenvdec";  }
        inline juce::String padFiltEnvSustain (int pad) { return "pad_" + juce::String (pad) + "_fenvsus";  }
        inline juce::String padFiltEnvRelease (int pad) { return "pad_" + juce::String (pad) + "_fenvrel";  }

        // Stereo shaping.
        inline juce::String padStereoWidth (int pad) { return "pad_" + juce::String (pad) + "_width";     }
        inline juce::String padMonoBelow   (int pad) { return "pad_" + juce::String (pad) + "_monobelow"; }

        // Velocity -> glide time, giving per-note control of slide length.
        inline juce::String padVelToGlide (int pad) { return "pad_" + juce::String (pad) + "_v2glide"; }

        // Snap incoming notes to the global key/scale (Keyboard mode only).
        inline juce::String padScaleLock (int pad) { return "pad_" + juce::String (pad) + "_scalelock"; }

        // Time-stretch / formant. Both are rendered offline into a processed
        // copy of the pad's sample — see engine/PhaseVocoder.h.
        inline juce::String padStretchMode (int pad) { return "pad_" + juce::String (pad) + "_stretchmode"; }
        inline juce::String padFormant     (int pad) { return "pad_" + juce::String (pad) + "_formant";     }

        // Hold formants fixed as the pad is played across the keyboard, instead
        // of letting them ride up and down with the resampling ratio.
        inline juce::String padFormantCorrect (int pad) { return "pad_" + juce::String (pad) + "_fcorrect"; }

        // Second LFO, LFO tempo sync and mod wheel routing.
        inline juce::String padLfoSync        (int pad) { return "pad_" + juce::String (pad) + "_lfosync";   }
        inline juce::String padLfo2Rate       (int pad) { return "pad_" + juce::String (pad) + "_lfo2rate";  }
        inline juce::String padLfo2Depth      (int pad) { return "pad_" + juce::String (pad) + "_lfo2depth"; }
        inline juce::String padLfo2Shape      (int pad) { return "pad_" + juce::String (pad) + "_lfo2shape"; }
        inline juce::String padLfo2Dest       (int pad) { return "pad_" + juce::String (pad) + "_lfo2dest";  }
        inline juce::String padLfo2Sync       (int pad) { return "pad_" + juce::String (pad) + "_lfo2sync";  }
        inline juce::String padModWheelDest   (int pad) { return "pad_" + juce::String (pad) + "_mwdest";    }
        inline juce::String padModWheelAmount (int pad) { return "pad_" + juce::String (pad) + "_mwamt";     }

        // Per-layer velocity range (Velocity layer mode; overlapping ranges
        // stack), trigger edge (press or release) and tuning. `layer` is 1-based.
        inline juce::String padLayerVelLow (int pad, int layer)
        {
            return "pad_" + juce::String (pad) + "_layvlo" + juce::String (layer);
        }
        inline juce::String padLayerVelHigh (int pad, int layer)
        {
            return "pad_" + juce::String (pad) + "_layvhi" + juce::String (layer);
        }
        inline juce::String padLayerTrigger (int pad, int layer)
        {
            return "pad_" + juce::String (pad) + "_laytrig" + juce::String (layer);
        }
        inline juce::String padLayerTranspose (int pad, int layer)
        {
            return "pad_" + juce::String (pad) + "_laytr" + juce::String (layer);
        }
        inline juce::String padLayerFine (int pad, int layer)
        {
            return "pad_" + juce::String (pad) + "_layfine" + juce::String (layer);
        }

        // Change length without changing pitch (rendered, like Stretch).
        inline juce::String padSpeed      (int pad) { return "pad_" + juce::String (pad) + "_speed";   }
        // Notes played away from the root keep the sample's length.
        inline juce::String padKeepLength (int pad) { return "pad_" + juce::String (pad) + "_keeplen"; }
    }

    class ASamplerProcessor : public juce::AudioProcessor,
                              public juce::ChangeBroadcaster,
                              public juce::AudioProcessorParameter::Listener,
                              private juce::Timer
    {
    public:
        ASamplerProcessor();
        ~ASamplerProcessor() override;

        void prepareToPlay (double sampleRate, int samplesPerBlock) override;
        void releaseResources() override;
        bool isBusesLayoutSupported (const BusesLayout&) const override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

        juce::AudioProcessorEditor* createEditor() override;
        bool hasEditor() const override { return true; }

        const juce::String getName() const override;
        bool acceptsMidi() const override                  { return true; }
        // Pads played from the computer keyboard are sent out as MIDI, so
        // they can be recorded on another track.
        bool producesMidi() const override                 { return true; }
        bool isMidiEffect() const override                 { return false; }
        // Longest possible tail across all pads (release + fade-out). Reporting
        // 0 here tells the host nothing sounds after the last note, and REAPER
        // will truncate release tails on render, freeze and bounce-in-place.
        double getTailLengthSeconds() const override;

        // Host programs: 0 is "Current" (whatever the user has set up, and a
        // no-op to select), 1..N are the factory kits. Exposing kits as
        // programs puts them in the host's preset menu and, through JUCE's
        // VST3 program parameter, in REAPER's FX parameter list, where a
        // screen reader can reach them without opening the editor.
        int getNumPrograms() override;
        int getCurrentProgram() override;
        void setCurrentProgram (int index) override;
        const juce::String getProgramName (int index) override;
        void changeProgramName (int, const juce::String&) override {}

        void getStateInformation (juce::MemoryBlock&) override;
        void setStateInformation (const void*, int) override;

        juce::AudioProcessorValueTreeState& getState() noexcept { return state; }

        // REAPER integration (record toggle, grabbing a timeline item). Set up
        // by the VST3 wrapper when the host is REAPER; empty otherwise.
        juce::VST3ClientExtensions* getVST3ClientExtensions() override;
        ReaperApi& getReaperApi() noexcept { return reaperApi; }

        // ---- Sample loading API (message thread) ----
        //
        // Outcome of a load attempt — exposed so the editor can announce results
        // to a screen reader in concrete terms.
        enum class LoadResult
        {
            Success,
            FileNotFound,
            FormatNotSupported,
            EmptyFile
        };

        // Load `file` into `padIndex` (0..kNumPads-1) at the currently selected
        // edit layer (the "Edit Layer" host parameter). Safe to swap while the
        // pad's previous sample is being played — the active voice keeps its
        // own strong reference until it finishes.
        LoadResult loadSampleForPad (int padIndex, const juce::File& file);

        // Explicit layer-targeted load. `layerIndex` must be in [0, kNumLayers).
        LoadResult loadSampleForPadLayer (int padIndex, int layerIndex,
                                          const juce::File& file);

        // Non-blocking load. Reads the file on a background thread and applies
        // the result on the message thread, announcing both the start and the
        // outcome. Used by the editor so a large sample never freezes the host
        // (a freeze is far worse without sight — there is nothing to look at
        // that says "still working").
        void loadSampleForPadLayerAsync (int padIndex, int layerIndex,
                                         const juce::File& file);

        // ---- Sample browsing ----
        //
        // Step the pad's current sample to the next (+1) or previous (-1)
        // audio file in the same folder, load it into the edit layer, play it,
        // and announce "name, N of M". This is how a sighted producer skims a
        // folder of kicks by ear; without it, every candidate costs a trip
        // through the file dialog. Wraps at either end (the position spoken
        // makes the wrap obvious). Asynchronous unless `synchronous` is true.
        enum class BrowseResult
        {
            Started,        // async load in flight (the outcome is announced)
            Success,        // synchronous load done
            NoCurrentFile,  // nothing loaded on the pad to browse from
            NoOtherFiles,   // the folder holds no other readable audio
            LoadFailed,
            InvalidPad
        };
        BrowseResult browseSample (int padIndex, int direction, bool synchronous = false);

        // Readable audio files in `folder`, in natural (human) sort order:
        // "kick 2" before "kick 10". Exposed for tests.
        juce::Array<juce::File> listAudioFilesIn (const juce::File& folder) const;

        // Repair pads whose saved sample paths no longer resolve, by searching
        // `searchDir` recursively for files of the same name. Returns how many
        // were relinked. This is what makes a project survive being moved to
        // another machine or a reorganised sample folder.
        int relinkMissingSamples (const juce::File& searchDir);

        // How many pad layers currently reference a sample that could not be
        // read. 0 when everything resolved.
        int getMissingSampleCount() const;

        // Set the layer's gain parameter so its peak sits at -1 dBFS. Adjusts
        // the parameter rather than the audio, so it stays non-destructive,
        // automatable and undoable. Returns false if the layer is empty/silent.
        bool normaliseLayer (int padIndex, int layerIndex);

        // Reset the current edit layer of `padIndex` to the built-in default.
        void resetPadToDefault (int padIndex);

        // Explicit layer-targeted reset.
        void resetPadLayerToDefault (int padIndex, int layerIndex);

        // ---- Chop / slice API (message thread) ----
        //
        // Distributes the sample currently on `sourcePadIndex` across the first
        // N pads (N = current "Chop Count" parameter, or the explicit `numSlices`
        // argument). Each destination pad receives the same Sample pointer and
        // gets its start/end parameters set to that slice's window. Destination
        // pads' previously loaded samples are replaced.
        enum class ChopResult
        {
            Success,
            SourcePadEmpty,    // source pad has no user-loaded sample
            InvalidSourcePad,
            InvalidSliceCount
        };

        ChopResult chopAcrossPads (int sourcePadIndex);
        ChopResult chopAcrossPads (int sourcePadIndex, int numSlices);
        ChopResult chopAcrossPads (int sourcePadIndex, int numSlices, ChopMode mode);

        // Current "Chop Count" parameter, resolved to its integer value.
        int getCurrentChopCount() const;

        // Current "Chop Mode" parameter, resolved to its enum value.
        ChopMode getCurrentChopMode() const;

        // 0-based index of the pad the Focus proxy group currently points at.
        int getCurrentFocusedPad() const;

        // Point the Focus group (and the editor's pad actions) at `padIndex`
        // (0-based), notifying the host but *not* recording an undo step:
        // walking the pad grid with the arrow keys is navigation, and Undo
        // must not step back through it.
        void setFocusedPad (int padIndex);

        // Mute / solo state, for spoken pad labels.
        bool isPadMuted  (int padIndex) const;
        bool isPadSoloed (int padIndex) const;

        // ---- Undo ----
        //
        // Auto-chop rewrites the start/end window on up to 16 pads at once.
        // Without undo that is unrecoverable, and it is far more punishing for
        // a user who cannot glance across the pads to see what changed.
        bool undoLastAction();
        bool redoLastAction();
        juce::UndoManager& getUndoManager() noexcept { return undoManager; }

        // ---- Kits ----
        //
        // A kit is the full parameter state plus the pads' sample paths, saved
        // as a standalone file so a set-up drum kit survives beyond one project.
        // Rebuilding a kit by hand is expensive for anyone; for a screen-reader
        // user it is the difference between using the plugin and abandoning it.
        bool saveKitToFile  (const juce::File& file);
        bool loadKitFromFile (const juce::File& file);

        // Built-in starting points. These configure envelopes, choke groups,
        // note layout and character — the setup work — and leave the samples to
        // the user, so they are useful without shipping any audio.
        static juce::StringArray getFactoryKitNames();

        // "1.0.0", from the CMake project version. Shown and spoken by the
        // editor so a support question can start with "which version?".
        static juce::String getVersionString();
        bool loadFactoryKit (int index);

        // ---- Pad utilities ----
        //
        // Copy every parameter of `padIndex` into an internal clipboard;
        // pastePad writes them onto another pad. `includeSample` also moves the
        // sample references, so "duplicate this pad" is one action.
        bool copyPad  (int padIndex);
        bool pastePad (int padIndex, bool includeSample = true);
        // A default-constructed ValueTree is "valid" but empty, so validity
        // alone would report a clipboard that has never been filled.
        bool hasCopiedPad() const noexcept
        {
            return copiedPad.isValid() && copiedPad.getNumProperties() > 0;
        }
        void clearAllPads();

        // ---- Slice -> MIDI ----
        //
        // Write a .mid file with one note per chopped slice, laid out on a
        // 16th-note grid at the host tempo. VST3 drag-out isn't accessible, so
        // writing a file the user then imports is both simpler and better here.
        bool exportSliceMidi (const juce::File& file, int numSlices);

        // Nudge the pad's Start/End onto the nearest detected onsets. Zero-cross
        // snap removes clicks but can't fix a boundary that auto-chop placed
        // slightly off the hit; this can. Undoable.
        bool snapPadWindowToTransients (int padIndex);

        // ---- Time-stretch / formant ----
        //
        // Renders are normally driven by a timer and run in the background.
        // This forces the pending work for one pad to happen now, on the
        // calling thread — used by tests, and available to any caller that
        // needs the processed audio to exist before it continues.
        void renderStretchNow (int padIndex);

        // True when `padIndex` is currently playing a stretched / formant
        // shifted render rather than its raw sample.
        bool isPadRendered (int padIndex, int layerIndex = 0) const;

        // Build the per-note formant-correction table for `padIndex` now, on
        // the calling thread, instead of waiting for the background render.
        void renderFormantTableNow (int padIndex);

        // How many formant-corrected variants `padIndex` currently holds for
        // the given layer. 0 when correction is off or not yet rendered.
        int getFormantVariantCount (int padIndex, int layerIndex = 0) const;

        // Number of background stretch/formant renders still running.
        int getPendingRenderCount() const noexcept
        {
            return pendingRenders.load (std::memory_order_relaxed);
        }

        // ---- Audition ----
        //
        // Trigger `padIndex` from the UI without any external MIDI. Thread-safe:
        // the request is picked up at the top of the next processBlock.
        // `announce` = false plays silently: the editor's pads and keys pass
        // that, since speaking "Audition Pad 5" over the sound itself only
        // gets in the way of hearing it.
        void auditionPad (int padIndex, float velocity = 0.8f, bool announce = true);

        // Play `padIndex` from `startNorm` (0..1 of the sample) for at most
        // `seconds`, without announcing anything. Used to let the user hear a
        // slice-point edit: a sighted user sees the boundary move; a blind user
        // has to hear it.
        void auditionPadFrom (int padIndex, float startNorm, double seconds);

        // Audition-on-edit. Called by the processor's timer: once a Start or
        // End edit on one pad has settled (no further change for ~200 ms), play
        // from the new Start, or the last ~300 ms before the new End. Only while
        // the transport is stopped (so automation never triggers it), only for
        // single-pad edits (a chop or kit load is not a slice edit), and only
        // with the "Audition On Edit" parameter on. Public for tests.
        void processEditAuditions();

        // For the editor: what's currently loaded on `padIndex` at the active
        // edit layer. Returns the file name (without path) or "default" if
        // that layer still holds the built-in tone.
        juce::String getPadSampleDisplayName (int padIndex) const;

        // The file behind the pad's edit layer (or its first loaded layer),
        // or an empty File when it holds the built-in tone.
        juce::File getPadSampleFile (int padIndex) const;

        // Explicit layer-targeted display name.
        juce::String getPadLayerSampleDisplayName (int padIndex, int layerIndex) const;

        // How `padIndex` is triggered, read from its live parameters, for
        // labels and announcements: "note C1" for a Fixed pad, "keys C1 to C4,
        // root C2" for a Keyboard pad. Labels must not assume the factory note
        // layout — a user who re-notes a pad would otherwise hear the old note.
        juce::String describePadTrigger (int padIndex) const;

        // Length in seconds of the sample the slice-point parameters refer to
        // on `padIndex` (0-based): the edit layer's sample if loaded, else the
        // first loaded layer, else the built-in tone. Lock-free; safe to call
        // from the host's parameter-text callbacks on any thread.
        double getPadReferenceSeconds (int padIndex) const;

        // "Describe sample": a spoken summary of what is on the pad, the facts
        // a sighted producer reads off the waveform at a glance. Name, played
        // length (and bars at the host tempo), channels and rate, peak level,
        // number of hits, and where the material allows it, tempo, key or
        // pitch. Analyses the pad's Start..End window on the edit layer. Sets
        // the status (so the editor announces it) and returns the text.
        juce::String describePad (int padIndex);

        // ---- Resample / bounce ----
        //
        // Render one hit of `padIndex` with everything the pad does to it
        // (envelope, filter and filter envelope, character, pitch, LFO,
        // stereo shaping), write it to a WAV under the per-user app-data
        // folder (ASampler/bounces), and load it onto the next empty pad.
        // The classic MPC resample: commit a sound, then chop or re-pitch the
        // result. Message thread; the render is offline and leaves the live
        // voices alone.
        struct BounceResult
        {
            bool       ok = false;
            juce::File file;            // the WAV written (if any)
            int        targetPad = -1;  // 0-based pad it was loaded onto, -1 if none was free
        };
        BounceResult bouncePad (int padIndex, const juce::File& outputDir = {});

        // Supplies per-pad sample lengths to the parameter formatters, which
        // are built before the pads exist.
        using PadLengthFn = std::function<double (int padIndex)>;

        // Current "Edit Layer" host parameter as a 0-based index.
        int getCurrentEditLayer() const;

        juce::String getLastStatusMessage() const;

        // Test/diagnostic helper: how many voices are currently sounding.
        int getActiveVoiceCount() const;
        // Test/diagnostic helper: is `padIndex` (0-based) sounding?
        bool isPadSounding (int padIndex) const;

        // Clamp a reader-reported sample count (int64) down to int. Anything
        // negative becomes 0; anything past INT_MAX is capped at INT_MAX.
        // Pulled out as a static helper so a test can hit the edges without
        // having to materialise a > 2 GB WAV file on disk.
        static int clampReaderLengthToInt (juce::int64 lengthInSamples) noexcept;

        // ---- MIDI learn API (message thread) ----
        //
        // Bind CC `cc` (0..127) to the parameter with ID `paramId`. Passing
        // an empty or unknown ID unbinds. CC values delivered in subsequent
        // processBlocks will write to that parameter via the host.
        void bindCcToParameter (int cc, const juce::String& paramId);

        // Convenience: unbind any parameter currently mapped to `cc`.
        void unbindCc (int cc);

        // Look up the parameter ID currently bound to `cc`, or "" if none.
        juce::String getCcBinding (int cc) const;

        // Most recently *gestured* parameter index (the one the user just
        // touched in the host's UI). -1 if nothing's been touched yet.
        // Used by the learn workflow to decide what to bind on the next
        // incoming CC while the arm parameter is on.
        int getLastTouchedParameterIndex() const;

        // ---- Pad names ----
        //
        // A name the user gives a pad ("Snare 2"). Spoken with the pad, saved
        // with the project and with kits. Empty = no name.
        void setPadName (int padIndex, const juce::String& name);
        juce::String getPadName (int padIndex) const;

        // ---- Apply a setting to many pads ----
        //
        // The per-pad controls, in Focus-group order, so the editor can offer
        // them by name. Index = position in that list.
        static int          getNumPadControls();
        static juce::String getPadControlName (int index);
        static juce::String getPadControlSection (int index);
        juce::String        getPadControlValueText (int padIndex, int controlIndex) const;

        // Copy one control's value (or every control in `section`) from
        // `sourcePad` to every pad in [firstPad, lastPad]. One undo step.
        // Returns how many pads changed.
        int applyControlToPads (int sourcePad, int controlIndex, int firstPad, int lastPad);
        int applySectionToPads (int sourcePad, const juce::String& section, int firstPad, int lastPad);

        // ---- Note learn ----
        //
        // The next MIDI note played sets the pad's Note (the root, in Keyboard
        // mode), Low Note or High Note. The note is consumed: it does not play.
        enum class NoteLearnTarget { Note = 0, LowNote = 1, HighNote = 2 };
        void armNoteLearn (int padIndex, NoteLearnTarget target);
        void cancelNoteLearn();
        bool isNoteLearnArmed() const noexcept { return noteLearnPad.load() >= 0; }
        // Applies a note the audio thread captured. Called by the timer; public
        // for tests.
        void processPendingNoteLearn();

        // ---- Loading many files at once ----
        enum class MultiLoadMode
        {
            OnePerPad,        // file k onto pad startPad + k (layer 1)
            RoundRobin,       // all onto startPad's layers, Layer Mode Round Robin
            VelocityLayers    // all onto startPad's layers, velocity ranges spread evenly
        };
        // Loads in the background (one read job, one announcement) unless
        // `synchronous`. Files beyond the last pad or layer are skipped and
        // the count is announced. Returns how many will be loaded.
        int loadSamples (int startPad, const juce::Array<juce::File>& files, MultiLoadMode mode,
                         bool synchronous = false);

        // Spread the velocity ranges of the pad's loaded press layers evenly
        // over 0..127, in layer order (soft to hard). Undoable.
        int spreadVelocityLayers (int padIndex);

        // ---- Kits: what a save includes ----
        enum class KitSaveMode
        {
            Paths,        // parameters + absolute sample paths (the default)
            WithSamples,  // also copies every sample into "<kit> samples" beside the kit
            Template      // parameters only; loading it keeps the current samples
        };
        bool saveKitToFile (const juce::File& file, KitSaveMode mode);

        // ---- Silence trimming ----
        //
        // Move the pad's Start past leading silence (anything 30 dB below the
        // peak) and its End to where the sound has died away (60 dB below),
        // measured on the edit layer's sample. Non-destructive and undoable.
        bool trimSilence (int padIndex);

        // ---- Raw preview player ----
        //
        // Play a buffer once on the main output, outside any pad (replacing
        // whatever preview was playing). Used for slice-point scrubbing.
        void playPreview (std::shared_ptr<const Sample> sample);
        void stopPreview();
        bool isPreviewPlaying() const noexcept { return previewActive.load(); }

        // ---- Slice-point scrubbing ----
        //
        // Play a short snippet that lets a slice point be placed by ear: ~80 ms
        // from a Start position, or the ~150 ms before an End position played
        // backwards. Does not change the parameter.
        void previewSlicePoint (int padIndex, bool isEnd, float norm);
        // Set Start or End (as one undo step) and announce it.
        void setSlicePoint (int padIndex, bool isEnd, float norm);
        float getSlicePoint (int padIndex, bool isEnd) const;

        // ---- Sample preview (try before you keep) ----
        //
        // Step through the audio files in the pad's folder (or `file`'s) and
        // play each candidate *on the pad*, with all its settings, without
        // replacing the pad's sample. While the transport runs the pad is not
        // auditioned: the candidate is simply what the pad plays in the beat.
        // keepPreview loads the candidate for real; cancelPreview goes back.
        BrowseResult previewSample (int padIndex, int direction, bool synchronous = false);
        BrowseResult previewSampleFile (int padIndex, const juce::File& file, bool synchronous = false);
        bool keepPreview (int padIndex);
        void cancelPreview (int padIndex);
        bool isPreviewing (int padIndex) const;
        juce::File getPreviewFile (int padIndex) const;

        // ---- Live notes (computer keyboard) ----
        //
        // A pad key pressed or released in the editor: played through the full
        // note dispatch, and sent to the plugin's MIDI output. Thread-safe.
        void queueLiveNote (int midiNote, bool isNoteOn);
        // The note a pad key plays: the pad's Note (its root in Keyboard mode).
        int  getPadPlayNote (int padIndex) const;

        // ---- REAPER ----
        bool toggleReaperRecord();
        // Render the one selected REAPER item (glue, with its fades, rate,
        // volume and take FX), copy the result to the app-data folder and load
        // it onto `padIndex`. REAPER's project is restored with an undo.
        bool grabReaperItem (int padIndex, const juce::File& outputDir = {});

        // juce::AudioProcessorParameter::Listener
        void parameterValueChanged    (int parameterIndex, float newValue) override;
        void parameterGestureChanged  (int parameterIndex, bool gestureIsStarting) override;

    private:
        struct PadLayer
        {
            AtomicSharedPtr<const Sample> sample;  // see engine/AtomicSharedPtr.h
            juce::String filePath;                              // message-thread only

            // Set when a saved path could not be read back (project moved to
            // another machine, sample folder relocated). The pad falls back to
            // the default sound, but we keep filePath so the reference
            // survives a re-save and relinkMissingSamples() can repair it —
            // silently forgetting the sample would lose the user's work.
            bool missing = false;

            // Time-stretched / formant-shifted rendering of `sample`, or null
            // when the pad's settings are neutral. The audio thread plays this
            // in preference to the raw sample, so stretching costs nothing per
            // block — the work happens once, off-thread, when settings change.
            AtomicSharedPtr<const Sample> processed;

            // What `processed` was rendered from. Message-thread only.
            double renderedTimeRatio = 1.0;
            double renderedFormant   = 0.0;

            // Bumped on every render request so a slow render that has been
            // superseded discards its result instead of overwriting a newer one.
            // Atomic because the render job polls it to stop early.
            std::atomic<int> renderGeneration { 0 };

            // Per-note formant-corrected variants (Keyboard mode only). Null
            // when correction is off. Takes precedence over `processed`, whose
            // single render cannot follow a per-note pitch.
            AtomicSharedPtr<const FormantTable> formantTable;

            // What the table was built for. Message-thread only.
            FormantTableKey  tableKey;
            std::atomic<int> tableGeneration { 0 };

            // Debounce for table rebuilds. Sweeping the Pitch knob walks the
            // required offset range a semitone at a time; without this, every
            // step in the sweep would queue a full table render.
            FormantTableKey pendingTableKey;
            int             pendingTableTicks = 0;
        };

        struct Pad
        {
            std::array<PadLayer, kNumLayers> layers;
        };

        static juce::AudioProcessorValueTreeState::ParameterLayout
            makeParameterLayout (const PadLengthFn& padSeconds);
        static BusesProperties makeBusesProperties();

        struct PadParamPtrs
        {
            std::atomic<float>* volume        = nullptr;
            std::atomic<float>* pan           = nullptr;
            std::atomic<float>* pitch         = nullptr;
            std::atomic<float>* note          = nullptr;
            std::atomic<float>* attack        = nullptr;
            std::atomic<float>* decay         = nullptr;
            std::atomic<float>* sustain       = nullptr;
            std::atomic<float>* release       = nullptr;
            std::atomic<float>* chokeGroup    = nullptr;
            std::atomic<float>* start            = nullptr;
            std::atomic<float>* end              = nullptr;
            std::atomic<float>* reverse          = nullptr;
            std::atomic<float>* velocityCurve    = nullptr;
            std::atomic<float>* filterMode       = nullptr;
            std::atomic<float>* filterCutoff     = nullptr;
            std::atomic<float>* filterResonance  = nullptr;
            std::atomic<float>* loopMode         = nullptr;
            std::atomic<float>* loopStart        = nullptr;
            std::atomic<float>* loopEnd          = nullptr;
            std::atomic<float>* layerMode        = nullptr;
            std::atomic<float>* repeatEnabled    = nullptr;
            std::atomic<float>* repeatRate       = nullptr;
            std::atomic<float>* oneShot          = nullptr;
            std::atomic<float>* mute             = nullptr;
            std::atomic<float>* solo             = nullptr;
            std::atomic<float>* glideMode        = nullptr;
            std::atomic<float>* glideTime        = nullptr;
            std::atomic<float>* maxVoices        = nullptr;
            std::atomic<float>* mode             = nullptr;
            std::atomic<float>* lowNote          = nullptr;
            std::atomic<float>* highNote         = nullptr;
            std::atomic<float>* satMode          = nullptr;
            std::atomic<float>* satAmount        = nullptr;
            std::atomic<float>* bitDepth         = nullptr;
            std::atomic<float>* downsample       = nullptr;
            std::atomic<float>* pitchEnvAmount   = nullptr;
            std::atomic<float>* pitchEnvTime     = nullptr;
            std::atomic<float>* fadeIn           = nullptr;
            std::atomic<float>* fadeOut          = nullptr;
            std::atomic<float>* loopXfade        = nullptr;
            std::atomic<float>* zeroSnap         = nullptr;
            std::atomic<float>* lfoRate          = nullptr;
            std::atomic<float>* lfoDepth         = nullptr;
            std::atomic<float>* lfoShape         = nullptr;
            std::atomic<float>* lfoDest          = nullptr;
            std::atomic<float>* velToCutoff      = nullptr;
            std::atomic<float>* velToPitch       = nullptr;
            std::atomic<float>* velToStart       = nullptr;
            std::atomic<float>* swing            = nullptr;
            std::atomic<float>* humanize         = nullptr;
            std::atomic<float>* tempoSync        = nullptr;
            std::atomic<float>* originalBpm      = nullptr;
            std::atomic<float>* velToGlide       = nullptr;
            std::atomic<float>* filtEnvAmount    = nullptr;
            std::atomic<float>* filtEnvAttack    = nullptr;
            std::atomic<float>* filtEnvDecay     = nullptr;
            std::atomic<float>* filtEnvSustain   = nullptr;
            std::atomic<float>* filtEnvRelease   = nullptr;
            std::atomic<float>* stereoWidth      = nullptr;
            std::atomic<float>* monoBelow        = nullptr;
            std::atomic<float>* scaleLock        = nullptr;
            std::atomic<float>* stretchMode      = nullptr;
            std::atomic<float>* formant          = nullptr;
            std::atomic<float>* formantCorrect    = nullptr;
            std::atomic<float>* lfoSync          = nullptr;
            std::atomic<float>* lfo2Rate         = nullptr;
            std::atomic<float>* lfo2Depth        = nullptr;
            std::atomic<float>* lfo2Shape        = nullptr;
            std::atomic<float>* lfo2Dest         = nullptr;
            std::atomic<float>* lfo2Sync         = nullptr;
            std::atomic<float>* modWheelDest     = nullptr;
            std::atomic<float>* modWheelAmount   = nullptr;
            std::array<std::atomic<float>*, kNumLayers> layerGain     {};
            std::array<std::atomic<float>*, kNumLayers> layerLowNote  {};
            std::array<std::atomic<float>*, kNumLayers> layerHighNote {};
            std::array<std::atomic<float>*, kNumLayers> layerRootNote {};
            std::array<std::atomic<float>*, kNumLayers> layerVelLow   {};
            std::array<std::atomic<float>*, kNumLayers> layerVelHigh  {};
            std::array<std::atomic<float>*, kNumLayers> layerTrigger  {};
            std::array<std::atomic<float>*, kNumLayers> layerTranspose {};
            std::array<std::atomic<float>*, kNumLayers> layerFine     {};
            std::atomic<float>* speed            = nullptr;
            std::atomic<float>* keepLength       = nullptr;
        };

        // Read an audio file into a Sample without touching any pad state, so
        // it is safe to call from the loader thread.
        LoadResult readSampleFile (const juce::File& file,
                                   std::shared_ptr<const Sample>& out) const;

        // Open a named undo step around a group of parameter writes.
        //
        // APVTS copies parameter values into its ValueTree on a timer, not
        // synchronously, and it is that copy the UndoManager records. So a
        // transaction opened before a batch of writes would otherwise capture
        // whatever the *previous* batch left pending. Flushing on both sides
        // pins the boundary: everything before this call belongs to the
        // previous step, everything after belongs to this one.
        void beginUndoTransaction (const juce::String& name);

        // Create the "value" property on every APVTS parameter node up front.
        // Without this the first write to a given parameter is not recorded by
        // the UndoManager — see the implementation for why.
        void materialiseParameterValueProperties();


        void cacheParamPointers();
        void regenerateDefaultSampleAtRate (double sampleRate);
        void setStatus (const juce::String& message);

        // Set a ranged host parameter to a plain (real-world) value and notify
        // the host, so the change shows in the FX list / automation, and lands in
        // the undo history.
        void setRangedParam (const juce::String& paramId, float plainValue);

        void handleNoteOn  (int midiNote, float velocity);
        void handleNoteOff (int midiNote);
        // Start the pad's release-trigger layers for `midiNote`, with the
        // velocity its note-on had.
        void triggerReleaseLayers (int padIndex, int midiNote);
        // Whether `midiNote` reaches pad `padIndex`. Fixed: the pad's Note.
        // Keyboard: anything in [Low Note, High Note]. Where the two overlap,
        // a Fixed pad holding a loaded sample keeps its note and the Keyboard
        // pad skips it; a Fixed pad still on the built-in tone steps aside
        // for the Keyboard pad. Either way one note sounds one thing.
        bool padRespondsToNote (int padIndex, int midiNote) const noexcept;
        bool keyboardPadCovers (int midiNote) const noexcept;

        // Does any layer of the pad hold something other than the built-in
        // tone? Safe on the audio thread (compares sample pointers rather
        // than reading the message-thread file path).
        bool padHoldsLoadedSample (int padIndex) const noexcept;
        void handlePitchBend (int wheelValue);
        // Resolve the per-pad mute/solo state into a gate in [0, 1]. Returns
        // 0.0f if the pad should be silenced this block, 1.0f otherwise.
        // Rule: if any pad is soloed, only non-muted soloed pads play; with
        // no solos, every non-muted pad plays. Cheap O(kNumPads) scan.
        float resolvePadGate (int padIndex) const;

        // Snapshot the live-modulatable parameters for `padIndex`. Called once
        // per pad per block and pushed onto that pad's sounding voices so
        // automation is audible mid-note. See VoiceLiveParams.
        VoiceLiveParams buildLiveParams (int padIndex) const;

        // Which layers a trigger plays, per the pad's Layer Mode, written to
        // `out` (room for kNumLayers); returns how many. Velocity mode returns
        // every loaded layer whose velocity range holds the hit, so overlapping
        // ranges stack; the other modes return one. `onRelease` picks among the
        // layers whose Trigger is Release (played at note-off) instead of the
        // Press ones. A pad with nothing loaded plays layer 0's built-in tone on
        // press. Resolved before pitch is computed because Key Zones mode
        // supplies the root note that the pitch offset is measured from.
        int selectLayersForTrigger (int padIndex, int midiNote, float velocity,
                                    bool onRelease, int* out) const;

        // The first layer selectLayersForTrigger would pick on press.
        int selectLayerForTrigger (int padIndex, int midiNote, float velocity) const;

        // Does this layer hold a user sample (not the built-in tone)? Audio
        // thread safe: compares sample pointers, never the file path.
        bool layerIsLoaded (int padIndex, int layerIndex) const noexcept;

        // Swap a layer's audio and invalidate everything derived from it (the
        // stretch render, the formant table and their keys). Every place that
        // replaces a layer's sample goes through here, so no path can leave a
        // stale render of the previous sample playing.
        void setLayerAudio (int padIndex, int layerIndex,
                            std::shared_ptr<const Sample> sample,
                            const juce::String& filePath, bool missing = false);

        // ---- Offline stretch / formant rendering ----
        //
        // Polls every pad for the stretch/formant settings it should currently
        // be rendered at, and kicks off a background render when that differs
        // from what it holds. A timer rather than a parameter listener because
        // the target also depends on host tempo, which no parameter reports.
        void timerCallback() override;

        // The render `padIndex` should currently have, derived from its
        // Stretch Mode / Tempo Sync / Original BPM / Formant settings and the
        // host tempo. Neutral options mean "play the raw sample".
        StretchOptions desiredStretchFor (int padIndex) const;

        // Start a background render of one layer, if one isn't already current.
        void requestRender (int padIndex, int layerIndex, const StretchOptions& options);

        // The formant table `padIndex`/`layerIndex` should currently hold,
        // derived from its Keyboard range, root and formant settings.
        FormantTableKey desiredFormantTableFor (int padIndex, int layerIndex) const;

        // Start a background build of one layer's formant-correction table.
        void requestFormantTable (int padIndex, int layerIndex, const FormantTableKey& key);

        // Render one table synchronously. Shared by the background job and the
        // force-now entry point so both produce identical results.
        // `shouldAbort` stops a superseded build early (the result is then
        // discarded). Variants render in parallel across the render pool's
        // share of cores.
        std::shared_ptr<const FormantTable>
            buildFormantTable (const Sample& source, const FormantTableKey& key,
                               const std::function<bool()>& shouldAbort = {}) const;

        // How many renders are in flight, so the UI can say so and tests can
        // wait for quiescence.
        std::atomic<int> pendingRenders { 0 };

        // Declared before `state` so it outlives the APVTS that references it.
        // Giving the APVTS an UndoManager is what makes every parameter change
        // undoable — including the ones auto-chop makes across other pads,
        // which are otherwise silently destructive.
        juce::UndoManager                  undoManager;

        juce::AudioProcessorValueTreeState state;
        juce::AudioFormatManager           formatManager;

        // Single background thread for sample reads. One thread (not a pool)
        // keeps loads in the order the user asked for them, which matters when
        // the only feedback is spoken.
        juce::ThreadPool                   loaderPool { 1 };

        // Stretch / formant renders. Separate from the loader so a long render
        // never holds up a sample load the user is waiting to hear, and with
        // several threads so pads re-render side by side after a tempo change.
        juce::ThreadPool                   renderPool { juce::ThreadPoolOptions{}
                                                            .withThreadName ("ASampler render")
                                                            .withNumberOfThreads (renderThreadCount()) };
        static int renderThreadCount() noexcept
        {
            return juce::jlimit (1, 4, juce::SystemStats::getNumCpus() - 1);
        }

        // Shared with callAsync callbacks queued by loader jobs. Cleared in the
        // destructor (message thread, as are the callbacks), so a load that
        // finishes just as the plugin closes is dropped instead of touching a
        // destroyed processor.
        std::shared_ptr<bool>              alive = std::make_shared<bool> (true);

        // Selected host program (see getNumPrograms). Saved with the project:
        // hosts commonly re-send the current program after restoring state,
        // and re-selecting the same program must not reload the kit over the
        // user's edits.
        std::atomic<int>                   currentProgram { 0 };

        // Background read + message-thread apply. `onDone` (optional) runs on
        // the message thread after the slot is updated and the status set.
        void startAsyncLoad (int padIndex, int layerIndex, const juce::File& file,
                             std::function<void (LoadResult)> onDone);

        // Message-thread completion shared by the sync and async browse paths.
        void finishBrowse (int padIndex, const juce::File& file, int position, int total);

        std::array<PadParamPtrs, kNumPads> padParams {};
        std::atomic<float>*                masterGainParam     = nullptr;
        std::atomic<float>*                chopCountParam      = nullptr;
        std::atomic<float>*                chopModeParam       = nullptr;
        std::atomic<float>*                editLayerParam      = nullptr;
        std::atomic<float>*                midiLearnArmParam   = nullptr;
        std::atomic<float>*                pitchBendRangeParam = nullptr;
        std::atomic<float>*                focusedPadParam     = nullptr;
        std::atomic<float>*                scaleKeyParam       = nullptr;
        std::atomic<float>*                scaleTypeParam      = nullptr;
        std::atomic<float>*                keyVelocityParam    = nullptr;
        std::atomic<float>*                trimOnLoadParam     = nullptr;

        // The Focus proxy parameters, in padParamSpecs() order. Owned by the
        // APVTS; these are non-owning pointers used to refresh every proxy
        // when the focused pad changes.
        std::vector<FocusProxyParameter*>  focusProxies;

        // Per-pad sample length last reported to the host (timer thread only),
        // so a swap that changes the slice-point text triggers a host refresh.
        std::array<double, kNumPads>       lastReportedPadSeconds {};

        // Clipboard for copyPad / pastePad. Holds plain (real-world) parameter
        // values keyed by the field suffix, plus the pad's sample paths.
        juce::ValueTree                    copiedPad { "copied_pad" };

        // Audition requests from the UI, consumed at the top of processBlock.
        // Value is velocity * 1000 (so it stays integral), or -1 for "none".
        std::array<std::atomic<int>, kNumPads> pendingAudition {};

        // Companions to pendingAudition, written before it (release order):
        // start override as 0..1 (negative = the pad's own Start), and how
        // long to hold the note before releasing it, in milliseconds.
        std::array<std::atomic<float>, kNumPads> pendingAuditionStart {};
        std::array<std::atomic<int>, kNumPads>   pendingAuditionHoldMs {};

        // Audio-thread audition state: samples left before the held audition
        // note is released, the note to release, and a start override that the
        // next trigger of the pad consumes.
        std::array<int, kNumPads>   auditionHoldRemaining {};
        std::array<int, kNumPads>   auditionNote {};
        std::array<float, kNumPads> auditionStartOverride {};

        // Audition-on-edit bookkeeping. Written from parameterValueChanged
        // (possibly the audio thread), so atomics only. Kind: 0 none,
        // 1 Start edited, 2 End edited.
        std::array<std::atomic<int>, kNumPads>         sliceEditKind {};
        std::array<std::atomic<juce::uint32>, kNumPads> sliceEditTimeMs {};
        std::vector<int>                               sliceEditLookup;   // param index -> pad*2 + (0 start | 1 end), or -1
        std::atomic<bool>                              transportPlaying { false };
        std::atomic<float>*                            auditionOnEditParam = nullptr;

        void queueAudition (int padIndex, float velocity, float startNorm, double holdSeconds);

        // Index of the "Focused Pad" parameter in getParameters(), cached so
        // parameterValueChanged can recognise it without a string compare on
        // what is potentially the audio thread. -1 until resolved.
        int                                focusedPadParamIndex = -1;

        // Current MIDI pitch bend (in semitones), updated from MIDI 0xE0
        // messages in processBlock. Audio-thread only.
        float                              currentPitchBendSemitones = 0.0f;
        // Last mod wheel (CC 1) position, 0..1. Atomic because bounce plans a
        // trigger on the message thread.
        std::atomic<float>                 currentModWheel { 0.0f };

        std::shared_ptr<const Sample>      defaultSample;
        std::array<Pad, kNumPads>          pads;

        // Per-pad rotating counter for LayerMode::RoundRobin. Audio-thread
        // only; not persisted (re-opening a project starts a fresh rotation).
        // Mutable because selectLayerForTrigger is logically const — it reports
        // which layer to use — but round-robin has to advance its counter as a
        // side effect of being asked.
        mutable std::array<std::atomic<int>, kNumPads> roundRobinIndex {};

        // Per-pad memory of the most recent trigger, used by the glide /
        // portamento path. `lastTriggerNote` is -1 until the first trigger
        // since prepareToPlay — that sentinel guards the "first note never
        // glides (no source pitch to come from)" rule. `lastTriggerPitchSt`
        // captures the pad's pitch-param value at trigger time; the delta
        // between that and the next trigger's pitch is what gets glided.
        // Reset in releaseResources so transport stops produce a clean slate.
        std::array<int,   kNumPads> lastTriggerNote {};
        std::array<float, kNumPads> lastTriggerPitchSt {};

        // Per-pad note-repeat tracking. Audio-thread only; cleared on
        // releaseResources so transport stops reset cleanly.
        struct NoteRepeatState
        {
            bool   held              = false;
            int    heldNote          = -1;    // MIDI note the user is currently
                                              // holding for this pad. In Fixed
                                              // mode this equals the pad's
                                              // assigned note; in Keyboard mode
                                              // it's whichever in-range key was
                                              // pressed, so repeats fire on the
                                              // held key, not the root.
            float  velocity          = 1.0f;
            double samplesUntilNext  = 0.0;   // negative = overshoot to handle
            int    stepCount         = 0;     // repeats emitted since this hold
                                              // began; drives swing (delay the
                                              // odd-numbered off-beats).
        };
        std::array<NoteRepeatState, kNumPads> noteRepeat {};

        // RNG for note-repeat humanize (timing + velocity jitter). Audio-thread
        // only, seeded once so rolls are deterministic within a session but not
        // identical every hit.
        juce::Random noteRepeatRandom;

        // Build a combined MIDI buffer from `input` + any synthetic note-on
        // repeats triggered by held pads, advancing the per-pad repeat clocks
        // for this block. Hold state is updated by user note-on/off events
        // only — synthetic repeats never touch it, so a repeating pad keeps
        // ticking until the user releases the original note.
        void buildCombinedMidi (const juce::MidiBuffer& input,
                                juce::MidiBuffer& output,
                                int numSamples);

        // Compute samples-per-step for a pad, using the current host BPM
        // (or 120 BPM as a fallback when no transport info is available).
        double samplesPerRepeatStep (int padIndex) const;

        // Host tempo, falling back to 120 BPM when the host reports none
        // (offline render, stopped transport in some hosts).
        double getHostBpm() const;
        std::atomic<double> cachedHostBpm { 0.0 };   // written by processBlock

        // Everything one hit of a pad needs: the voice settings, the note the
        // voice is filed under (for note-off), and the sample to play. Shared
        // by live triggers and offline bouncing so both sound the same.
        // `liveTrigger` = false leaves the audio-thread state (glide history,
        // pitch bend, audition overrides) untouched and unused, so it is safe
        // to call from the message thread.
        struct TriggerPlan
        {
            VoiceConfig                    cfg;
            int                            triggerNote = 0;
            std::shared_ptr<const Sample>  sample;
            VoiceConfig                    baseCfg;   // cfg before the layer was applied
        };
        TriggerPlan planTrigger (int padIndex, int midiNote, float velocity,
                                 int selectedLayer, bool liveTrigger);

        // The same hit on another layer of the pad: `base` is the plan's config
        // before any layer-specific part (gain trim, transpose, sample) was
        // applied. Used for stacked velocity layers, so they all share one
        // glide, pitch and start.
        TriggerPlan planLayer (int padIndex, int layerIndex, VoiceConfig base, int triggerNote) const;

        // Per-pad, per-note velocity of the last note-on, so a release-trigger
        // layer plays with the dynamics the key was struck with. Audio thread.
        std::array<std::array<float, 128>, kNumPads> noteOnVelocity {};

        // Pre-scan input MIDI for CC events: if armed and the last-touched
        // parameter is valid, bind the CC to it and disarm. Then apply each
        // CC value to its bound parameter (if any) via the host.
        void handleMidiLearn (const juce::MidiBuffer& input);

        // CC -> parameter-index map. -1 = unbound. Audio thread loads;
        // message thread stores. atomic<int> on every modern target is
        // lock-free, so the audio side stays real-time clean.
        std::array<std::atomic<int>, 128> ccBindings {};

        // Index (in getParameters()) of the parameter the user most recently
        // *gestured* on in the host UI. -1 = nothing yet. Audio thread reads
        // when a CC arrives while armed; the listener writes from whatever
        // thread the host fires gestures on (usually message).
        std::atomic<int> lastTouchedParameterIndex { -1 };

        // Re-entry guard for parameterGestureChanged: when we call
        // setValueNotifyingHost from the MIDI dispatch path it would
        // otherwise look just like a user touch and clobber lastTouched.
        std::atomic<bool> dispatchingMidiLearn { false };

        VoicePool                          voicePool;

        juce::String                       lastStatus;

        // Pad names. Message thread.
        std::array<juce::String, kNumPads> padNames;

        // Note learn: armed pad (-1 = off) and target; the audio thread writes
        // the captured note, the timer applies it.
        std::atomic<int>                   noteLearnPad { -1 };
        std::atomic<int>                   noteLearnTarget { 0 };
        std::atomic<int>                   learnedNote { -1 };
        int                                noteLearnSwallowOff = -1;   // audio thread

        // Raw preview player (audio thread reads, message thread swaps).
        AtomicSharedPtr<const Sample>      previewSampleBuffer;
        std::atomic<int>                   previewGeneration { 0 };
        std::atomic<bool>                  previewActive { false };
        int                                previewPlayedGeneration = 0;   // audio thread
        double                             previewPosition = 0.0;         // audio thread

        // Try-before-you-keep: a candidate sample the pad plays in place of
        // its edit layer, and the file it came from.
        std::array<AtomicSharedPtr<const Sample>, kNumPads> previewOverride;
        std::array<juce::String, kNumPads>                  previewFilePath;   // message thread

        // Live notes from the editor, single producer (message thread), single
        // consumer (audio thread).
        juce::AbstractFifo                 liveNoteFifo { 256 };
        std::array<int, 256>               liveNoteData {};   // note | (on ? 0x100 : 0)

        // Folder of the last sample loaded anywhere, so previewing on an empty
        // pad has somewhere to start.
        juce::File                         lastSampleFolder;

        ReaperApi                          reaperApi;
        std::unique_ptr<juce::VST3ClientExtensions> vst3Extensions;

        juce::File resolveKitSamplePath (const juce::File& kitFile, const juce::ValueTree& entry) const;
        void maybeTrimOnLoad (int padIndex, int layerIndex);
        bool trimSilenceImpl (int padIndex, std::shared_ptr<const Sample> sample,
                              bool wholeSample, bool announce);
        int  spreadVelocityLayersImpl (int padIndex, bool announce);
        void applyMultiLoad (int startPad, MultiLoadMode mode, const juce::Array<juce::File>& files,
                             const std::vector<std::shared_ptr<const Sample>>& samples);

        // Preview buffers the audio thread may still be reading. Kept here so
        // the last reference is never dropped on the audio thread.
        std::vector<std::shared_ptr<const Sample>> previewRetired;
        juce::ValueTree padNamesToTree() const;
        void padNamesFromTree (const juce::ValueTree& names);

        // Shared by previewSample / previewSampleFile: read `file` and make it
        // the pad's candidate.
        BrowseResult startPreview (int padIndex, const juce::File& file, int position, int total,
                                   bool synchronous);
        void applyPreview (int padIndex, const juce::File& file, std::shared_ptr<const Sample> sample,
                           int position, int total);

        // The sample slice points refer to on a pad (edit layer, else first
        // loaded layer, else the built-in tone).
        std::shared_ptr<const Sample> referenceSample (int padIndex) const;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ASamplerProcessor)
    };
}
