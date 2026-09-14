#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "VocalSynth.h"

#include "engine/DefaultSample.h"
#include "engine/PhaseVocoder.h"
#include "engine/Transient.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace asampler
{
    namespace
    {
        constexpr int defaultNoteForPad (int padOneBased) noexcept
        {
            return 35 + padOneBased;
        }

        juce::String formatDb (float v)         { return juce::String (v, 2) + " dB"; }

        juce::String formatPan (float v)
        {
            if (std::abs (v) < 0.005f) return juce::String ("C");
            const int pct = static_cast<int> (std::round (std::abs (v) * 100.0f));
            return (v < 0.0f ? juce::String ("L") : juce::String ("R")) + juce::String (pct);
        }

        juce::String formatSemitones (float v)
        {
            const auto prefix = (v >= 0.0f) ? juce::String ("+") : juce::String();
            return prefix + juce::String (v, 2) + " st";
        }

        juce::String formatNote (int note)
        {
            return juce::MidiMessage::getMidiNoteName (note, true, true, 3);
        }

        juce::String formatTime (float seconds)
        {
            if (seconds < 1.0f)
                return juce::String (static_cast<int> (std::round (seconds * 1000.0f))) + " ms";
            return juce::String (seconds, 2) + " s";
        }

        juce::String formatPercent (float zeroToOne)
        {
            return juce::String (static_cast<int> (std::round (zeroToOne * 100.0f))) + "%";
        }

        juce::String formatChokeGroup (int group)
        {
            return group == 0 ? juce::String ("None") : ("Group " + juce::String (group));
        }

        // Choice indices map 1:1 onto the VelocityCurve enum — keep these
        // ordered identically to enum class VelocityCurve in VelocityCurve.h.
        const juce::StringArray kVelocityCurveLabels { "Fixed", "Soft", "Linear", "Hard" };

        // Same contract as kVelocityCurveLabels, but for FilterMode.
        const juce::StringArray kFilterModeLabels { "Off", "Low-pass", "High-pass", "Band-pass" };

        // Same contract — keep aligned with enum class LoopMode.
        const juce::StringArray kLoopModeLabels { "Off", "Forward", "Ping-Pong" };

        // Chop count choices — must mirror chopCountFromIndex() below; default
        // is the last entry (16 slices spans all 16 pads). 1:1 with the
        // AudioParameterChoice index stored in saved state.
        const juce::StringArray kChopCountLabels { "2", "4", "8", "16" };

        constexpr int chopCountFromIndex (int choiceIndex) noexcept
        {
            switch (choiceIndex)
            {
                case 0: return 2;
                case 1: return 4;
                case 2: return 8;
                case 3: return 16;
                default: return 16;
            }
        }

        // Chop mode labels — keep aligned with enum class ChopMode in
        // PluginProcessor.h. AudioParameterChoice serialises by index.
        const juce::StringArray kChopModeLabels { "Equal",
                                                  "Transients",
                                                  "Transients (Spectral)" };

        // Edit-layer labels for the global "Edit Layer" host parameter. 1-based
        // for the screen reader; the parameter's integer value is 0-based.
        const juce::StringArray kEditLayerLabels { "Layer 1", "Layer 2",
                                                   "Layer 3", "Layer 4" };
        static_assert (kNumLayers == 4,
                       "Edit-layer labels must match kNumLayers");

        // Per-pad "Layer Mode" labels — keep aligned with enum class LayerMode
        // in PluginProcessor.h. AudioParameterChoice serialises by index.
        const juce::StringArray kLayerModeLabels { "Velocity",
                                                   "Round Robin",
                                                   "Random",
                                                   "Key Zones" };

        // Note-repeat division labels — keep aligned with enum class
        // NoteRepeatRate in PluginProcessor.h. T = triplet feel.
        const juce::StringArray kRepeatRateLabels {
            "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"
        };

        // Per-pad aftertouch destination labels — keep aligned with
        // enum class AftertouchDest in PluginProcessor.h. AudioParameterChoice
        // serialises by index.
        const juce::StringArray kAftertouchDestLabels { "Off",
                                                        "Volume",
                                                        "Filter Cutoff" };

        // Per-pad glide-mode labels — keep aligned with enum class GlideMode
        // in PluginProcessor.h. AudioParameterChoice serialises by index.
        const juce::StringArray kGlideModeLabels { "Off", "Legato", "Always",
                                                   "Legato (rate)", "Always (rate)" };

        // Musical scales for the key/scale lock — keep aligned with enum class
        // Scale in PluginProcessor.h. AudioParameterChoice serialises by index.
        const juce::StringArray kScaleLabels { "Chromatic", "Major", "Natural Minor",
                                               "Harmonic Minor", "Pentatonic Minor",
                                               "Pentatonic Major", "Blues", "Dorian",
                                               "Phrygian", "Mixolydian" };

        // Per-pad tempo reconciliation — keep aligned with enum class
        // StretchMode in PluginProcessor.h.
        const juce::StringArray kStretchModeLabels { "Repitch", "Stretch" };

        const juce::StringArray kKeyLabels { "C", "C#", "D", "D#", "E", "F",
                                             "F#", "G", "G#", "A", "A#", "B" };

        // Per-pad note-dispatch mode labels — keep aligned with enum class
        // PadMode in PluginProcessor.h. AudioParameterChoice serialises by index.
        const juce::StringArray kPadModeLabels { "Fixed", "Keyboard" };

        // Saturation-mode labels — keep aligned with enum class SaturationMode
        // in engine/Saturation.h. AudioParameterChoice serialises by index.
        const juce::StringArray kSaturationModeLabels { "Tube", "Tape", "Hard Fold" };

        // LFO shape / destination labels — keep aligned with enum class LfoShape
        // and enum class LfoDest in engine/Lfo.h.
        const juce::StringArray kLfoShapeLabels { "Sine", "Triangle", "Square", "Saw" };
        const juce::StringArray kLfoDestLabels  { "Off", "Pitch", "Filter Cutoff",
                                                  "Volume", "Pan" };

        constexpr int stepsPerBeatForRate (NoteRepeatRate r) noexcept
        {
            switch (r)
            {
                case NoteRepeatRate::Quarter:             return 1;
                case NoteRepeatRate::Eighth:              return 2;
                case NoteRepeatRate::EighthTriplet:       return 3;
                case NoteRepeatRate::Sixteenth:           return 4;
                case NoteRepeatRate::SixteenthTriplet:    return 6;
                case NoteRepeatRate::ThirtySecond:        return 8;
                case NoteRepeatRate::ThirtySecondTriplet: return 12;
            }
            return 4;
        }

        // Velocity → layer index. Splits 0..127 into kNumLayers equal-width
        // bands; the top band absorbs velocity 127. Caller is responsible for
        // the "this layer isn't loaded, fall back to a loaded one" search.
        int layerForVelocity (float velocityNormalised) noexcept
        {
            const int v127 = juce::jlimit (
                0, 127, static_cast<int> (std::round (velocityNormalised * 127.0f)));
            constexpr int bandSize = 128 / kNumLayers;
            return juce::jlimit (0, kNumLayers - 1, v127 / bandSize);
        }

        juce::String formatHz (float hz)
        {
            if (hz < 1000.0f)
                return juce::String (static_cast<int> (std::round (hz))) + " Hz";
            return juce::String (hz / 1000.0f, 2) + " kHz";
        }

        juce::String formatResonance (float q)
        {
            return "Q " + juce::String (q, 2);
        }


        // Every per-pad parameter, paired with the display name its Focus proxy
        // uses. Kept in makePadGroup order so the Focus group reads in the same
        // sequence as a pad group — muscle memory transfers between the two.
        //
        // Adding a per-pad parameter means adding it here too, otherwise it is
        // reachable only by scrolling the full per-pad list — an accessibility
        // regression. The test suite asserts the two stay in step.
        struct PadParamSpec
        {
            juce::String (*idFor) (int);   // 1-based pad index -> parameter id
            const char*   name;
        };

        const std::vector<PadParamSpec>& padParamSpecs()
        {
            static const std::vector<PadParamSpec> specs = {
                { [] (int p) { return param_ids::padVolume (p); }, "Volume" },
                { [] (int p) { return param_ids::padPan (p); }, "Pan" },
                { [] (int p) { return param_ids::padPitch (p); }, "Pitch" },
                { [] (int p) { return param_ids::padNote (p); }, "Note" },
                { [] (int p) { return param_ids::padMode (p); }, "Mode" },
                { [] (int p) { return param_ids::padLowNote (p); }, "Low Note" },
                { [] (int p) { return param_ids::padHighNote (p); }, "High Note" },
                { [] (int p) { return param_ids::padAttack (p); }, "Attack" },
                { [] (int p) { return param_ids::padDecay (p); }, "Decay" },
                { [] (int p) { return param_ids::padSustain (p); }, "Sustain" },
                { [] (int p) { return param_ids::padRelease (p); }, "Release" },
                { [] (int p) { return param_ids::padChokeGroup (p); }, "Choke Group" },
                { [] (int p) { return param_ids::padStart (p); }, "Start" },
                { [] (int p) { return param_ids::padEnd (p); }, "End" },
                { [] (int p) { return param_ids::padReverse (p); }, "Reverse" },
                { [] (int p) { return param_ids::padVelocityCurve (p); }, "Velocity Curve" },
                { [] (int p) { return param_ids::padFilterMode (p); }, "Filter Mode" },
                { [] (int p) { return param_ids::padFilterCutoff (p); }, "Filter Cutoff" },
                { [] (int p) { return param_ids::padFilterResonance (p); }, "Filter Resonance" },
                { [] (int p) { return param_ids::padLoopMode (p); }, "Loop Mode" },
                { [] (int p) { return param_ids::padLoopStart (p); }, "Loop Start" },
                { [] (int p) { return param_ids::padLoopEnd (p); }, "Loop End" },
                { [] (int p) { return param_ids::padLayerMode (p); }, "Layer Mode" },
                { [] (int p) { return param_ids::padRepeatEnabled (p); }, "Repeat" },
                { [] (int p) { return param_ids::padRepeatRate (p); }, "Repeat Rate" },
                { [] (int p) { return param_ids::padOneShot (p); }, "One Shot" },
                { [] (int p) { return param_ids::padAftertouchDest (p); }, "AT Destination" },
                { [] (int p) { return param_ids::padAftertouchAmount (p); }, "AT Amount" },
                { [] (int p) { return param_ids::padMute (p); }, "Mute" },
                { [] (int p) { return param_ids::padSolo (p); }, "Solo" },
                { [] (int p) { return param_ids::padGlideMode (p); }, "Glide Mode" },
                { [] (int p) { return param_ids::padGlideTime (p); }, "Glide Time" },
                { [] (int p) { return param_ids::padMaxVoices (p); }, "Max Voices" },
                { [] (int p) { return param_ids::padSatMode (p); }, "Saturation Mode" },
                { [] (int p) { return param_ids::padSatAmount (p); }, "Saturation" },
                { [] (int p) { return param_ids::padBitDepth (p); }, "Bit Depth" },
                { [] (int p) { return param_ids::padDownsample (p); }, "SR Reduce" },
                { [] (int p) { return param_ids::padPitchEnvAmount (p); }, "Pitch Env Amount" },
                { [] (int p) { return param_ids::padPitchEnvTime (p); }, "Pitch Env Time" },
                { [] (int p) { return param_ids::padFadeIn (p); }, "Fade In" },
                { [] (int p) { return param_ids::padFadeOut (p); }, "Fade Out" },
                { [] (int p) { return param_ids::padLoopXfade (p); }, "Loop Crossfade" },
                { [] (int p) { return param_ids::padZeroSnap (p); }, "Zero-Cross Snap" },
                { [] (int p) { return param_ids::padLfoRate (p); }, "LFO Rate" },
                { [] (int p) { return param_ids::padLfoDepth (p); }, "LFO Depth" },
                { [] (int p) { return param_ids::padLfoShape (p); }, "LFO Shape" },
                { [] (int p) { return param_ids::padLfoDest (p); }, "LFO Destination" },
                { [] (int p) { return param_ids::padVelToCutoff (p); }, "Vel to Cutoff" },
                { [] (int p) { return param_ids::padVelToPitch (p); }, "Vel to Pitch" },
                { [] (int p) { return param_ids::padVelToStart (p); }, "Vel to Start" },
                { [] (int p) { return param_ids::padSwing (p); }, "Swing" },
                { [] (int p) { return param_ids::padHumanize (p); }, "Humanize" },
                { [] (int p) { return param_ids::padTempoSync (p); }, "Tempo Sync" },
                { [] (int p) { return param_ids::padOriginalBpm (p); }, "Original BPM" },
                { [] (int p) { return param_ids::padVelToGlide (p); }, "Vel to Glide" },
                { [] (int p) { return param_ids::padFiltEnvAmount (p); }, "Filter Env Amount" },
                { [] (int p) { return param_ids::padFiltEnvAttack (p); }, "Filter Env Attack" },
                { [] (int p) { return param_ids::padFiltEnvDecay (p); }, "Filter Env Decay" },
                { [] (int p) { return param_ids::padFiltEnvSustain (p); }, "Filter Env Sustain" },
                { [] (int p) { return param_ids::padFiltEnvRelease (p); }, "Filter Env Release" },
                { [] (int p) { return param_ids::padStereoWidth (p); }, "Stereo Width" },
                { [] (int p) { return param_ids::padMonoBelow (p); }, "Mono Below" },
                { [] (int p) { return param_ids::padScaleLock (p); }, "Scale Lock" },
                { [] (int p) { return param_ids::padStretchMode (p); }, "Stretch Mode" },
                { [] (int p) { return param_ids::padFormant (p); }, "Formant" },
                { [] (int p) { return param_ids::padFormantCorrect (p); }, "Formant Correct" },
                { [] (int p) { return param_ids::padLayerGain (p, 1); }, "Layer 1 Gain" },
                { [] (int p) { return param_ids::padLayerGain (p, 2); }, "Layer 2 Gain" },
                { [] (int p) { return param_ids::padLayerGain (p, 3); }, "Layer 3 Gain" },
                { [] (int p) { return param_ids::padLayerGain (p, 4); }, "Layer 4 Gain" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 1); }, "Layer 1 Low Note" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 1); }, "Layer 1 High Note" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 1); }, "Layer 1 Root Note" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 2); }, "Layer 2 Low Note" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 2); }, "Layer 2 High Note" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 2); }, "Layer 2 Root Note" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 3); }, "Layer 3 Low Note" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 3); }, "Layer 3 High Note" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 3); }, "Layer 3 Root Note" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 4); }, "Layer 4 Low Note" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 4); }, "Layer 4 High Note" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 4); }, "Layer 4 Root Note" },
            };
            return specs;
        }

        std::unique_ptr<juce::AudioProcessorParameterGroup> makePadGroup (int pad)
        {
            using namespace juce;

            auto group = std::make_unique<AudioProcessorParameterGroup> (
                "pad_" + String (pad), "Pad " + String (pad), "|");

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVolume (pad), 1 }, "Volume",
                NormalisableRange<float> { -60.0f, 12.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("dB")
                    .withStringFromValueFunction ([] (float v, int) { return formatDb (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padPan (pad), 1 }, "Pan",
                NormalisableRange<float> { -1.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPan (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padPitch (pad), 1 }, "Pitch",
                NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("st")
                    .withStringFromValueFunction ([] (float v, int) { return formatSemitones (v); })));

            group->addChild (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padNote (pad), 1 }, "Note",
                0, 127, defaultNoteForPad (pad),
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

            // Note-dispatch mode. Default Fixed preserves every existing kit:
            // the pad fires only on `Note`. Keyboard mode re-purposes `Note`
            // as the root and listens across [Low Note, High Note] — incoming
            // notes pitch the sample relative to root, so one pad can cover
            // an 808's full range while neighbouring pads stay on fixed drum
            // notes in the same kit.
            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padMode (pad), 1 }, "Mode",
                kPadModeLabels,
                static_cast<int> (PadMode::Fixed)));

            // Range bounds used only in Keyboard mode. Defaults span the full
            // MIDI keyboard so flipping a pad to Keyboard without further
            // tweaks produces an instrument that responds to every key.
            group->addChild (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padLowNote (pad), 1 }, "Low Note",
                0, 127, 0,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

            group->addChild (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padHighNote (pad), 1 }, "High Note",
                0, 127, 127,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padAttack (pad), 1 }, "Attack",
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padDecay (pad), 1 }, "Decay",
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padSustain (pad), 1 }, "Sustain",
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 1.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padRelease (pad), 1 }, "Release",
                NormalisableRange<float> { 0.0f, 10.0f, 0.001f, 0.4f }, 0.050f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            group->addChild (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padChokeGroup (pad), 1 }, "Choke Group",
                0, 16, 0,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return formatChokeGroup (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padStart (pad), 1 }, "Start",
                NormalisableRange<float> { 0.0f, 1.0f, 0.0001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padEnd (pad), 1 }, "End",
                NormalisableRange<float> { 0.0f, 1.0f, 0.0001f }, 1.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            group->addChild (std::make_unique<AudioParameterBool> (
                ParameterID { param_ids::padReverse (pad), 1 }, "Reverse", false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padVelocityCurve (pad), 1 }, "Velocity Curve",
                kVelocityCurveLabels,
                static_cast<int> (VelocityCurve::Linear)));

            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padFilterMode (pad), 1 }, "Filter Mode",
                kFilterModeLabels,
                static_cast<int> (FilterMode::Off)));

            // Cutoff is log-skewed so half-travel sits near a musically useful
            // frequency. skew of 0.3 lands ~700 Hz at 50%, a common SVF default.
            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFilterCutoff (pad), 1 }, "Filter Cutoff",
                NormalisableRange<float> { 20.0f, 20000.0f, 1.0f, 0.3f }, 20000.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("Hz")
                    .withStringFromValueFunction ([] (float v, int) { return formatHz (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFilterResonance (pad), 1 }, "Filter Resonance",
                NormalisableRange<float> { 0.1f, 10.0f, 0.01f, 0.5f }, 0.707f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatResonance (v); })));

            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLoopMode (pad), 1 }, "Loop Mode",
                kLoopModeLabels,
                static_cast<int> (LoopMode::Off)));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLoopStart (pad), 1 }, "Loop Start",
                NormalisableRange<float> { 0.0f, 1.0f, 0.0001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLoopEnd (pad), 1 }, "Loop End",
                NormalisableRange<float> { 0.0f, 1.0f, 0.0001f }, 1.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLayerMode (pad), 1 }, "Layer Mode",
                kLayerModeLabels,
                static_cast<int> (LayerMode::Velocity)));

            group->addChild (std::make_unique<AudioParameterBool> (
                ParameterID { param_ids::padRepeatEnabled (pad), 1 }, "Repeat", false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padRepeatRate (pad), 1 }, "Repeat Rate",
                kRepeatRateLabels,
                static_cast<int> (NoteRepeatRate::Sixteenth)));

            // One-shot mode: when on, note-off is ignored and the voice
            // plays to the end of its sample window (or loop end, if looped).
            // The MPC default for drums - kicks, snares, claps should not
            // release their tail when the user lifts the key.
            group->addChild (std::make_unique<AudioParameterBool> (
                ParameterID { param_ids::padOneShot (pad), 1 }, "One Shot", false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            // Aftertouch routing per pad. Channel pressure (MIDI 0xD0) is
            // tracked globally on the processor; each pad chooses whether
            // and how much it responds. Default: Off so legacy projects
            // ignore aftertouch the way the old build did.
            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padAftertouchDest (pad), 1 }, "AT Destination",
                kAftertouchDestLabels,
                static_cast<int> (AftertouchDest::Off)));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padAftertouchAmount (pad), 1 }, "AT Amount",
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.5f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            // Mute and Solo. Both are simple booleans the user can flip from
            // REAPER's FX param list to dial in a beat without rewiring choke
            // groups. Solo overrides mute across pads (the standard DAW rule):
            // if any pad is soloed, only soloed pads play, regardless of the
            // mute state of other pads.
            group->addChild (std::make_unique<AudioParameterBool> (
                ParameterID { param_ids::padMute (pad), 1 }, "Mute", false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            group->addChild (std::make_unique<AudioParameterBool> (
                ParameterID { param_ids::padSolo (pad), 1 }, "Solo", false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            // Glide / portamento. Off by default so existing kits don't get
            // unexpected slides. Legato is the para-monophonic synth norm:
            // glide only when the previous voice is still sounding. Always
            // glides from the pad's most recent trigger note regardless.
            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padGlideMode (pad), 1 }, "Glide Mode",
                kGlideModeLabels,
                static_cast<int> (GlideMode::Off)));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padGlideTime (pad), 1 }, "Glide Time",
                NormalisableRange<float> { 0.0f, 2000.0f, 0.1f, 0.4f }, 100.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            // Per-pad voice cap. 1 = monophonic (a defining trap-bass setting:
            // each retrigger steals the previous voice). 8 = the global pool
            // size divided by 4, which is plenty for percussive layering and
            // matches the prior implicit behaviour for typical drum work.
            group->addChild (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padMaxVoices (pad), 1 }, "Max Voices",
                1, 8, 8,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return juce::String (v); })));

            // ---- Saturation / bit-crush character chain ----
            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padSatMode (pad), 1 }, "Saturation Mode",
                kSaturationModeLabels,
                static_cast<int> (SaturationMode::Tube)));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padSatAmount (pad), 1 }, "Saturation",
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            // Bit depth: 16 = bypass (clean), lower = grittier. Displayed as a
            // bit count; 16 reads "Off" so the screen reader is unambiguous.
            group->addChild (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padBitDepth (pad), 1 }, "Bit Depth",
                2, 16, 16,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int)
                        { return v >= 16 ? juce::String ("Off")
                                         : juce::String (v) + " bit"; })));

            // Sample-rate reduction: divide the effective rate by this factor.
            // 1 = bypass; higher = more aliasing / lo-fi character.
            group->addChild (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padDownsample (pad), 1 }, "SR Reduce",
                1, 64, 1,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int)
                        { return v <= 1 ? juce::String ("Off")
                                        : (juce::String ("/") + juce::String (v)); })));

            // ---- Pitch envelope ----
            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padPitchEnvAmount (pad), 1 }, "Pitch Env Amount",
                NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("st")
                    .withStringFromValueFunction ([] (float v, int) { return formatSemitones (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padPitchEnvTime (pad), 1 }, "Pitch Env Time",
                NormalisableRange<float> { 0.0f, 2000.0f, 0.1f, 0.4f }, 80.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            // ---- Slice cleanup: fades, loop crossfade, zero-cross snap ----
            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFadeIn (pad), 1 }, "Fade In",
                NormalisableRange<float> { 0.0f, 500.0f, 0.1f, 0.5f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFadeOut (pad), 1 }, "Fade Out",
                NormalisableRange<float> { 0.0f, 500.0f, 0.1f, 0.5f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLoopXfade (pad), 1 }, "Loop Crossfade",
                NormalisableRange<float> { 0.0f, 500.0f, 0.1f, 0.5f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            group->addChild (std::make_unique<AudioParameterBool> (
                ParameterID { param_ids::padZeroSnap (pad), 1 }, "Zero-Cross Snap", false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            // ---- Per-pad LFO ----
            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLfoRate (pad), 1 }, "LFO Rate",
                NormalisableRange<float> { 0.01f, 20.0f, 0.01f, 0.4f }, 1.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("Hz")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 2) + " Hz"; })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLfoDepth (pad), 1 }, "LFO Depth",
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLfoShape (pad), 1 }, "LFO Shape",
                kLfoShapeLabels,
                static_cast<int> (LfoShape::Sine)));

            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLfoDest (pad), 1 }, "LFO Destination",
                kLfoDestLabels,
                static_cast<int> (LfoDest::Off)));

            // ---- Velocity modulation ----
            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVelToCutoff (pad), 1 }, "Vel to Cutoff",
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVelToPitch (pad), 1 }, "Vel to Pitch",
                NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("st")
                    .withStringFromValueFunction ([] (float v, int) { return formatSemitones (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVelToStart (pad), 1 }, "Vel to Start",
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            // ---- Note-repeat feel ----
            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padSwing (pad), 1 }, "Swing",
                NormalisableRange<float> { 0.0f, 0.75f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padHumanize (pad), 1 }, "Humanize",
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            // ---- Tempo sync ----
            group->addChild (std::make_unique<AudioParameterBool> (
                ParameterID { param_ids::padTempoSync (pad), 1 }, "Tempo Sync", false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? "On" : "Off"; })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padOriginalBpm (pad), 1 }, "Original BPM",
                NormalisableRange<float> { 40.0f, 300.0f, 0.01f }, 120.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("BPM")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 2) + " BPM"; })));

            // ---- Velocity -> glide time ----
            //
            // Gives per-note control of slide length straight from the piano
            // roll: a hard hit snaps, a soft hit slides. At 0 every slide on
            // the pad uses the same Glide Time.
            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVelToGlide (pad), 1 }, "Vel to Glide",
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            // ---- Filter envelope ----
            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvAmount (pad), 1 }, "Filter Env Amount",
                NormalisableRange<float> { -8.0f, 8.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("oct")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 2) + " oct"; })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvAttack (pad), 1 }, "Filter Env Attack",
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.001f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvDecay (pad), 1 }, "Filter Env Decay",
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.200f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvSustain (pad), 1 }, "Filter Env Sustain",
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvRelease (pad), 1 }, "Filter Env Release",
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.100f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            // ---- Stereo shaping ----
            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padStereoWidth (pad), 1 }, "Stereo Width",
                NormalisableRange<float> { 0.0f, 2.0f, 0.01f }, 1.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int)
                    {
                        if (v < 0.005f)               return juce::String ("Mono");
                        if (std::abs (v - 1.0f) < 0.005f) return juce::String ("Normal");
                        return juce::String (static_cast<int> (std::round (v * 100.0f))) + "%";
                    })));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padMonoBelow (pad), 1 }, "Mono Below",
                NormalisableRange<float> { 0.0f, 500.0f, 1.0f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("Hz")
                    .withStringFromValueFunction ([] (float v, int)
                    {
                        if (v < 1.0f) return juce::String ("Off");
                        return juce::String (static_cast<int> (v)) + " Hz";
                    })));

            // ---- Time-stretch / formant ----
            group->addChild (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padStretchMode (pad), 1 }, "Stretch Mode",
                kStretchModeLabels, static_cast<int> (StretchMode::Repitch)));

            group->addChild (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFormant (pad), 1 }, "Formant",
                NormalisableRange<float> { -12.0f, 12.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("st")
                    .withStringFromValueFunction ([] (float v, int)
                    {
                        if (std::abs (v) < 0.005f) return juce::String ("Off");
                        return formatSemitones (v);
                    })));

            group->addChild (std::make_unique<AudioParameterBool> (
                ParameterID { param_ids::padFormantCorrect (pad), 1 },
                "Formant Correct", false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? "On" : "Off"; })));

            // ---- Key / scale lock ----
            group->addChild (std::make_unique<AudioParameterBool> (
                ParameterID { param_ids::padScaleLock (pad), 1 }, "Scale Lock", false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? "On" : "Off"; })));

            // ---- Per-layer gain trim and key mapping ----
            for (int L = 1; L <= kNumLayers; ++L)
            {
                group->addChild (std::make_unique<AudioParameterFloat> (
                    ParameterID { param_ids::padLayerGain (pad, L), 1 },
                    "Layer " + juce::String (L) + " Gain",
                    NormalisableRange<float> { -24.0f, 24.0f, 0.1f }, 0.0f,
                    AudioParameterFloatAttributes()
                        .withLabel ("dB")
                        .withStringFromValueFunction ([] (float v, int)
                                                       { return formatDb (v); })));

                // Key-zone mapping. Defaults span the whole keyboard with a
                // root of C3, so switching Layer Mode to Key Zones without
                // touching these behaves like the single-sample case.
                group->addChild (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerLowNote (pad, L), 1 },
                    "Layer " + juce::String (L) + " Low Note",
                    0, 127, 0,
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

                group->addChild (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerHighNote (pad, L), 1 },
                    "Layer " + juce::String (L) + " High Note",
                    0, 127, 127,
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

                group->addChild (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerRootNote (pad, L), 1 },
                    "Layer " + juce::String (L) + " Root Note",
                    0, 127, 60,
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));
            }

            return group;
        }
    }

    // One Main bus plus one stereo bus per pad. The per-pad buses default to
    // disabled so a fresh REAPER project sees the familiar 2-channel plugin
    // and only sprouts extra outputs when the user opts in.
    juce::AudioProcessor::BusesProperties
    ASamplerProcessor::makeBusesProperties()
    {
        BusesProperties b;
        b.addBus (false, "Main", juce::AudioChannelSet::stereo(), true);
        for (int p = 1; p <= kNumPads; ++p)
            b.addBus (false, "Pad " + juce::String (p),
                      juce::AudioChannelSet::stereo(), false);
        return b;
    }

    ASamplerProcessor::ASamplerProcessor()
        : AudioProcessor (makeBusesProperties()),
          state (*this, &undoManager, "STATE", makeParameterLayout())
    {
        formatManager.registerBasicFormats();

        defaultSample = std::make_shared<const Sample> (generateDefaultSample (44100.0));
        for (auto& pad : pads)
            for (auto& layer : pad.layers)
                layer.sample.store (defaultSample);

        for (auto& binding : ccBindings)
            binding.store (-1, std::memory_order_relaxed);

        lastTriggerNote.fill (-1);
        lastTriggerPitchSt.fill (0.0f);

        cacheParamPointers();
        materialiseParameterValueProperties();

        // Poll for stretch/formant work a few times a second. Cheap (a handful
        // of atomic loads per pad) and it picks up host tempo changes, which no
        // parameter callback would report.
        startTimer (250);

        // Subscribe to every parameter so we can track which one the user
        // most recently *gestured* on in the host UI — the basis of the
        // "arm-then-touch-then-move-CC" learn workflow.
        for (auto* p : getParameters())
            p->addListener (this);
    }

    ASamplerProcessor::~ASamplerProcessor()
    {
        stopTimer();

        // Stop the loader before anything it captures goes away. Jobs capture
        // `this`, so outliving the processor would be a use-after-free.
        loaderPool.removeAllJobs (true, 5000);

        for (auto* p : getParameters())
            p->removeListener (this);
    }

    juce::AudioProcessorValueTreeState::ParameterLayout
    ASamplerProcessor::makeParameterLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { param_ids::masterGain, 1 }, "Master Gain",
            NormalisableRange<float> { -60.0f, 12.0f, 0.01f }, 0.0f,
            AudioParameterFloatAttributes()
                .withLabel ("dB")
                .withStringFromValueFunction ([] (float v, int) { return formatDb (v); })));

        // Global "Chop Count" choice — used by chopAcrossPads. Default is 16
        // (last index), matching the most common MPC workflow of chopping a
        // 1-bar loop into 16ths.
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { param_ids::chopCount, 1 }, "Chop Count",
            kChopCountLabels,
            kChopCountLabels.size() - 1));

        // "Chop Mode" — Equal (default) gives reproducible 1/N slicing; the
        // Transients option places slice boundaries at detected onsets and
        // falls back to Equal when the source doesn't have enough.
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { param_ids::chopMode, 1 }, "Chop Mode",
            kChopModeLabels,
            static_cast<int> (ChopMode::Equal)));

        // "Edit Layer" — selects which velocity layer the editor's Load /
        // Reset buttons act on. Exposed to the host so a screen-reader user
        // can switch layers from REAPER's FX parameter list when the GUI
        // is collapsed. Default index 0 = Layer 1 (the primary layer).
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { param_ids::editLayer, 1 }, "Edit Layer",
            kEditLayerLabels, 0));

        // "MIDI Learn" — when on, the next incoming CC binds to the parameter
        // the user most recently touched in the host UI, then auto-disarms.
        // Exposing this as a host parameter means a screen-reader user can
        // arm it from REAPER's FX param list without touching the editor.
        layout.add (std::make_unique<AudioParameterBool> (
            ParameterID { param_ids::midiLearnArm, 1 }, "MIDI Learn", false,
            AudioParameterBoolAttributes()
                .withStringFromValueFunction ([] (bool v, int)
                                               { return v ? juce::String ("Armed") : juce::String ("Off"); })));

        // Global "Pitch Bend Range" — how many semitones full-positive bend
        // (MIDI value 16383) shifts every sounding voice up by; full-negative
        // bend shifts down by the same. Default 2 st matches the MIDI 1.0
        // recommendation; 12 / 24 are common for synth-style leads.
        layout.add (std::make_unique<AudioParameterInt> (
            ParameterID { param_ids::pitchBendRange, 1 }, "Pitch Bend Range",
            1, 24, 2,
            AudioParameterIntAttributes()
                .withStringFromValueFunction ([] (int v, int)
                                               { return juce::String (v) + " st"; })));

        // "Vocal Target Pad" — which pad the editor's SAPI5 vocal-generation
        // buttons write into. A host parameter so a screen-reader user can pick
        // the target from REAPER's FX list without touching the GUI.
        layout.add (std::make_unique<AudioParameterInt> (
            ParameterID { param_ids::vocalTargetPad, 1 }, "Vocal Target Pad",
            1, kNumPads, 1,
            AudioParameterIntAttributes()
                .withStringFromValueFunction ([] (int v, int)
                                               { return "Pad " + juce::String (v); })));

        // Global musical key + scale. Any pad with Scale Lock on snaps incoming
        // notes to these, so a chopped melodic sample can be played across the
        // keyboard without landing off-key.
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { param_ids::scaleKey, 1 }, "Key", kKeyLabels, 0));

        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { param_ids::scaleType, 1 }, "Scale", kScaleLabels,
            static_cast<int> (Scale::Chromatic)));

        // "Focused Pad" — which pad the Focus proxy group below points at.
        // Placed immediately before the Focus group so that in REAPER's FX
        // list the selector sits directly above the parameters it retargets.
        layout.add (std::make_unique<AudioParameterInt> (
            ParameterID { param_ids::focusedPad, 1 }, "Focused Pad",
            1, kNumPads, 1,
            AudioParameterIntAttributes()
                .withStringFromValueFunction ([] (int v, int)
                                               { return "Pad " + juce::String (v); })));

        // The Focus group: one mirrored copy of every per-pad parameter,
        // forwarding to whichever pad "Focused Pad" selects. This is what keeps
        // the plugin navigable by screen reader — see FocusProxyParameter.h for
        // the full rationale.
        {
            auto focusGroup = std::make_unique<AudioProcessorParameterGroup> (
                "focus", "Focus", "|");

            for (const auto& spec : padParamSpecs())
                focusGroup->addChild (
                    std::make_unique<FocusProxyParameter> (spec.name, kNumPads));

            layout.add (std::move (focusGroup));
        }

        for (int p = 1; p <= kNumPads; ++p)
            layout.add (makePadGroup (p));

        return layout;
    }

    void ASamplerProcessor::cacheParamPointers()
    {
        masterGainParam     = state.getRawParameterValue (param_ids::masterGain);
        chopCountParam      = state.getRawParameterValue (param_ids::chopCount);
        chopModeParam       = state.getRawParameterValue (param_ids::chopMode);
        editLayerParam      = state.getRawParameterValue (param_ids::editLayer);
        midiLearnArmParam   = state.getRawParameterValue (param_ids::midiLearnArm);
        pitchBendRangeParam = state.getRawParameterValue (param_ids::pitchBendRange);
        vocalTargetPadParam = state.getRawParameterValue (param_ids::vocalTargetPad);

        for (int i = 0; i < kNumPads; ++i)
        {
            const int p = i + 1;
            padParams[i].volume     = state.getRawParameterValue (param_ids::padVolume (p));
            padParams[i].pan        = state.getRawParameterValue (param_ids::padPan (p));
            padParams[i].pitch      = state.getRawParameterValue (param_ids::padPitch (p));
            padParams[i].note       = state.getRawParameterValue (param_ids::padNote (p));
            padParams[i].attack     = state.getRawParameterValue (param_ids::padAttack (p));
            padParams[i].decay      = state.getRawParameterValue (param_ids::padDecay (p));
            padParams[i].sustain    = state.getRawParameterValue (param_ids::padSustain (p));
            padParams[i].release       = state.getRawParameterValue (param_ids::padRelease (p));
            padParams[i].chokeGroup    = state.getRawParameterValue (param_ids::padChokeGroup (p));
            padParams[i].start            = state.getRawParameterValue (param_ids::padStart (p));
            padParams[i].end              = state.getRawParameterValue (param_ids::padEnd (p));
            padParams[i].reverse          = state.getRawParameterValue (param_ids::padReverse (p));
            padParams[i].velocityCurve    = state.getRawParameterValue (param_ids::padVelocityCurve (p));
            padParams[i].filterMode       = state.getRawParameterValue (param_ids::padFilterMode (p));
            padParams[i].filterCutoff     = state.getRawParameterValue (param_ids::padFilterCutoff (p));
            padParams[i].filterResonance  = state.getRawParameterValue (param_ids::padFilterResonance (p));
            padParams[i].loopMode         = state.getRawParameterValue (param_ids::padLoopMode (p));
            padParams[i].loopStart        = state.getRawParameterValue (param_ids::padLoopStart (p));
            padParams[i].loopEnd          = state.getRawParameterValue (param_ids::padLoopEnd (p));
            padParams[i].layerMode        = state.getRawParameterValue (param_ids::padLayerMode (p));
            padParams[i].repeatEnabled    = state.getRawParameterValue (param_ids::padRepeatEnabled (p));
            padParams[i].repeatRate       = state.getRawParameterValue (param_ids::padRepeatRate (p));
            padParams[i].oneShot          = state.getRawParameterValue (param_ids::padOneShot (p));
            padParams[i].aftertouchDest   = state.getRawParameterValue (param_ids::padAftertouchDest (p));
            padParams[i].aftertouchAmount = state.getRawParameterValue (param_ids::padAftertouchAmount (p));
            padParams[i].mute             = state.getRawParameterValue (param_ids::padMute (p));
            padParams[i].solo             = state.getRawParameterValue (param_ids::padSolo (p));
            padParams[i].glideMode        = state.getRawParameterValue (param_ids::padGlideMode (p));
            padParams[i].glideTime        = state.getRawParameterValue (param_ids::padGlideTime (p));
            padParams[i].maxVoices        = state.getRawParameterValue (param_ids::padMaxVoices (p));
            padParams[i].mode             = state.getRawParameterValue (param_ids::padMode (p));
            padParams[i].lowNote          = state.getRawParameterValue (param_ids::padLowNote (p));
            padParams[i].highNote         = state.getRawParameterValue (param_ids::padHighNote (p));
            padParams[i].satMode          = state.getRawParameterValue (param_ids::padSatMode (p));
            padParams[i].satAmount        = state.getRawParameterValue (param_ids::padSatAmount (p));
            padParams[i].bitDepth         = state.getRawParameterValue (param_ids::padBitDepth (p));
            padParams[i].downsample       = state.getRawParameterValue (param_ids::padDownsample (p));
            padParams[i].pitchEnvAmount   = state.getRawParameterValue (param_ids::padPitchEnvAmount (p));
            padParams[i].pitchEnvTime     = state.getRawParameterValue (param_ids::padPitchEnvTime (p));
            padParams[i].fadeIn           = state.getRawParameterValue (param_ids::padFadeIn (p));
            padParams[i].fadeOut          = state.getRawParameterValue (param_ids::padFadeOut (p));
            padParams[i].loopXfade        = state.getRawParameterValue (param_ids::padLoopXfade (p));
            padParams[i].zeroSnap         = state.getRawParameterValue (param_ids::padZeroSnap (p));
            padParams[i].lfoRate          = state.getRawParameterValue (param_ids::padLfoRate (p));
            padParams[i].lfoDepth         = state.getRawParameterValue (param_ids::padLfoDepth (p));
            padParams[i].lfoShape         = state.getRawParameterValue (param_ids::padLfoShape (p));
            padParams[i].lfoDest          = state.getRawParameterValue (param_ids::padLfoDest (p));
            padParams[i].velToCutoff      = state.getRawParameterValue (param_ids::padVelToCutoff (p));
            padParams[i].velToPitch       = state.getRawParameterValue (param_ids::padVelToPitch (p));
            padParams[i].velToStart       = state.getRawParameterValue (param_ids::padVelToStart (p));
            padParams[i].swing            = state.getRawParameterValue (param_ids::padSwing (p));
            padParams[i].humanize         = state.getRawParameterValue (param_ids::padHumanize (p));
            padParams[i].tempoSync        = state.getRawParameterValue (param_ids::padTempoSync (p));
            padParams[i].originalBpm      = state.getRawParameterValue (param_ids::padOriginalBpm (p));

            padParams[i].velToGlide     = state.getRawParameterValue (param_ids::padVelToGlide (p));
            padParams[i].filtEnvAmount  = state.getRawParameterValue (param_ids::padFiltEnvAmount (p));
            padParams[i].filtEnvAttack  = state.getRawParameterValue (param_ids::padFiltEnvAttack (p));
            padParams[i].filtEnvDecay   = state.getRawParameterValue (param_ids::padFiltEnvDecay (p));
            padParams[i].filtEnvSustain = state.getRawParameterValue (param_ids::padFiltEnvSustain (p));
            padParams[i].filtEnvRelease = state.getRawParameterValue (param_ids::padFiltEnvRelease (p));
            padParams[i].stereoWidth    = state.getRawParameterValue (param_ids::padStereoWidth (p));
            padParams[i].monoBelow      = state.getRawParameterValue (param_ids::padMonoBelow (p));
            padParams[i].scaleLock      = state.getRawParameterValue (param_ids::padScaleLock (p));
            padParams[i].stretchMode    = state.getRawParameterValue (param_ids::padStretchMode (p));
            padParams[i].formant        = state.getRawParameterValue (param_ids::padFormant (p));
            padParams[i].formantCorrect = state.getRawParameterValue (param_ids::padFormantCorrect (p));

            for (int L = 0; L < kNumLayers; ++L)
            {
                padParams[i].layerGain[(std::size_t) L] =
                    state.getRawParameterValue (param_ids::padLayerGain (p, L + 1));
                padParams[i].layerLowNote[(std::size_t) L] =
                    state.getRawParameterValue (param_ids::padLayerLowNote (p, L + 1));
                padParams[i].layerHighNote[(std::size_t) L] =
                    state.getRawParameterValue (param_ids::padLayerHighNote (p, L + 1));
                padParams[i].layerRootNote[(std::size_t) L] =
                    state.getRawParameterValue (param_ids::padLayerRootNote (p, L + 1));
            }
        }

        scaleKeyParam  = state.getRawParameterValue (param_ids::scaleKey);
        scaleTypeParam = state.getRawParameterValue (param_ids::scaleType);

        focusedPadParam = state.getRawParameterValue (param_ids::focusedPad);

        // Second phase of the Focus proxies' setup: the proxies were created
        // in the parameter layout, but their targets only exist once the APVTS
        // has finished constructing. Resolve every target now so nothing on a
        // hot path ever does a string lookup.
        //
        // Proxies were added to the layout in padParamSpecs() order, and
        // AudioProcessorValueTreeState preserves that ordering, so walking the
        // spec list and the proxy list together keeps them aligned.
        focusProxies.clear();
        focusProxies.reserve (padParamSpecs().size());

        for (auto* p : getParameters())
        {
            auto* proxy = dynamic_cast<FocusProxyParameter*> (p);
            if (proxy == nullptr)
                continue;

            const auto index = focusProxies.size();
            if (index >= padParamSpecs().size())
                break;   // more proxies than specs would mean the two drifted

            const auto& spec = padParamSpecs()[index];

            std::vector<juce::RangedAudioParameter*> targets;
            targets.reserve ((std::size_t) kNumPads);
            for (int pad = 1; pad <= kNumPads; ++pad)
                targets.push_back (state.getParameter (spec.idFor (pad)));

            proxy->setTargets (std::move (targets), focusedPadParam);
            focusProxies.push_back (proxy);
        }

        jassert (focusProxies.size() == padParamSpecs().size());

        focusedPadParamIndex = -1;
        if (auto* fp = state.getParameter (param_ids::focusedPad))
            focusedPadParamIndex = fp->getParameterIndex();
    }

    int ASamplerProcessor::getCurrentFocusedPad() const
    {
        if (focusedPadParam == nullptr) return 0;
        // Parameter is 1-based ("Pad 1".."Pad 16"); expose a 0-based index.
        return juce::jlimit (0, kNumPads - 1,
                             juce::roundToInt (focusedPadParam->load()) - 1);
    }

    void ASamplerProcessor::regenerateDefaultSampleAtRate (double sampleRate)
    {
        auto newDefault = std::make_shared<const Sample> (generateDefaultSample (sampleRate));
        defaultSample = newDefault;

        // Per-layer: any slot still on the previous default adopts the new-rate
        // one; user-loaded slots keep their own sample.
        for (auto& pad : pads)
            for (auto& layer : pad.layers)
                if (layer.filePath.isEmpty())
                    layer.sample.store (newDefault);
    }

    void ASamplerProcessor::setStatus (const juce::String& message)
    {
        lastStatus = message;
        sendChangeMessage();
    }

    double ASamplerProcessor::getTailLengthSeconds() const
    {
        // The worst case a host needs to keep rendering for is the longest
        // per-pad release plus that pad's fade-out. Params are read without
        // locking — a torn read here only produces a slightly stale tail
        // estimate, never a correctness problem.
        double longest = 0.0;
        for (const auto& p : padParams)
        {
            if (p.release == nullptr)
                continue;

            double tail = static_cast<double> (p.release->load());
            if (p.fadeOut != nullptr)
                tail += static_cast<double> (p.fadeOut->load()) * 0.001;   // ms -> s

            longest = juce::jmax (longest, tail);
        }

        // Floor of 1s covers the case where params aren't cached yet (the host
        // may ask before prepareToPlay) without over-reporting.
        return juce::jmax (1.0, longest);
    }

    void ASamplerProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
    {
        voicePool.prepare (sampleRate, kNumVoices);
        regenerateDefaultSampleAtRate (sampleRate);
    }

    void ASamplerProcessor::releaseResources()
    {
        voicePool.reset();

        // Clear note-repeat hold state so transport stops / re-prepares don't
        // resurrect a held pad with stale velocity / clock.
        for (auto& s : noteRepeat)
        {
            s.held             = false;
            s.heldNote         = -1;
            s.samplesUntilNext = 0.0;
            s.stepCount        = 0;
        }

        // Centre the pitch bend so any voice triggered after a transport stop
        // doesn't inherit the previous session's wheel position.
        currentPitchBendSemitones = 0.0f;
        currentAftertouch         = 0.0f;

        // Forget the last-triggered note per pad so the first note after a
        // transport stop never glides.
        lastTriggerNote.fill (-1);
        lastTriggerPitchSt.fill (0.0f);
    }

    int ASamplerProcessor::getActiveVoiceCount() const
    {
        return voicePool.countActive();
    }

    double ASamplerProcessor::getHostBpm() const
    {
        // Pull tempo from the host. If there's no transport info (offline
        // render, transport stopped in some hosts), fall back to 120 BPM so
        // tempo-dependent features still behave predictably.
        double bpm = 120.0;
        if (auto* head = const_cast<ASamplerProcessor*> (this)->getPlayHead())
        {
            if (auto pos = head->getPosition())
                if (auto b = pos->getBpm())
                    bpm = *b;
        }
        return (bpm > 0.0) ? bpm : 120.0;
    }

    double ASamplerProcessor::samplesPerRepeatStep (int padIndex) const
    {
        const double bpm = getHostBpm();

        const auto rate = static_cast<NoteRepeatRate> (
            juce::roundToInt (padParams[(std::size_t) padIndex].repeatRate->load()));
        const double secondsPerBeat = 60.0 / bpm;
        const double sr = (getSampleRate() > 0.0) ? getSampleRate() : 44100.0;
        return secondsPerBeat * sr / static_cast<double> (stepsPerBeatForRate (rate));
    }

    void ASamplerProcessor::buildCombinedMidi (const juce::MidiBuffer& input,
                                               juce::MidiBuffer& output,
                                               int numSamples)
    {
        // Advance each held pad's clock across [from, to). When the clock
        // crosses zero, emit a synthetic note-on at the appropriate sample
        // position and reset by samplesPerStep.
        auto tickClocks = [this, &output] (int from, int to)
        {
            if (to <= from) return;
            const int delta = to - from;

            for (int p = 0; p < kNumPads; ++p)
            {
                auto& s = noteRepeat[(std::size_t) p];
                if (! s.held) continue;
                if (padParams[(std::size_t) p].repeatEnabled->load() < 0.5f) continue;

                const double step = samplesPerRepeatStep (p);
                if (step <= 0.0) continue;

                const float swing    = padParams[(std::size_t) p].swing->load();     // 0..1
                const float humanize = padParams[(std::size_t) p].humanize->load();  // 0..1

                s.samplesUntilNext -= static_cast<double> (delta);
                int safetyGuard = 32;  // upper bound on repeats-per-block

                while (s.samplesUntilNext <= 0.0 && safetyGuard-- > 0)
                {
                    // Timing feel applied as a per-hit offset only — never folded
                    // back into samplesUntilNext — so the average tempo stays
                    // locked to the grid. Swing pushes the odd-numbered off-beats
                    // later (full swing = half a step, a triplet-ish shuffle);
                    // humanize adds symmetric jitter up to +/- a quarter step.
                    double feelOffset = 0.0;
                    if ((s.stepCount & 1) != 0)
                        feelOffset += static_cast<double> (swing) * 0.5 * step;
                    if (humanize > 0.0f)
                        feelOffset += (noteRepeatRandom.nextDouble() * 2.0 - 1.0)
                                    * static_cast<double> (humanize) * 0.25 * step;

                    // samplesUntilNext is <= 0 here, so the grid position is in
                    // [from, to). The repeat fires at to + overshoot + feel.
                    const int emitAt = juce::jlimit (from, to - 1,
                        to + static_cast<int> (std::round (s.samplesUntilNext + feelOffset)));

                    // Humanize the velocity too so rolls breathe instead of
                    // machine-gunning at a fixed level.
                    float vel = s.velocity;
                    if (humanize > 0.0f)
                        vel = juce::jlimit (0.02f, 1.0f,
                            vel + static_cast<float> (noteRepeatRandom.nextDouble() * 2.0 - 1.0)
                                * humanize * 0.3f);

                    // Emit on the *held* note so Keyboard-mode pads roll the
                    // pressed key, not the root. Fixed-mode pads stored the
                    // assigned note as heldNote at note-on time, so this
                    // collapses to the previous behaviour for them.
                    output.addEvent (juce::MidiMessage::noteOn (1, s.heldNote, vel),
                                     emitAt);
                    s.samplesUntilNext += step;
                    ++s.stepCount;
                }
            }
        };

        int prev = 0;
        for (const auto meta : input)
        {
            const int eventSample = juce::jlimit (0, numSamples, meta.samplePosition);
            tickClocks (prev, eventSample);
            prev = eventSample;

            const auto msg = meta.getMessage();
            output.addEvent (msg, eventSample);

            // Hold state updates come from user MIDI only — synthetic repeats
            // emitted above never reach this branch, so a repeating pad keeps
            // its rhythm intact until the user releases the original note.
            if (msg.isNoteOn())
            {
                const int   note = msg.getNoteNumber();
                const float vel  = msg.getFloatVelocity();
                for (int p = 0; p < kNumPads; ++p)
                {
                    const auto padMode = static_cast<PadMode> (
                        juce::roundToInt (padParams[(std::size_t) p].mode->load()));
                    bool matches = false;
                    if (padMode == PadMode::Keyboard)
                    {
                        const int lo = juce::roundToInt (
                            padParams[(std::size_t) p].lowNote->load());
                        const int hi = juce::roundToInt (
                            padParams[(std::size_t) p].highNote->load());
                        matches = (note >= lo && note <= hi);
                    }
                    else
                    {
                        matches = (juce::roundToInt (
                            padParams[(std::size_t) p].note->load()) == note);
                    }
                    if (! matches) continue;
                    auto& s = noteRepeat[(std::size_t) p];
                    s.held     = true;
                    s.heldNote = note;
                    s.velocity = vel;
                    s.samplesUntilNext = samplesPerRepeatStep (p);
                    s.stepCount = 0;   // restart swing parity on a fresh hold
                }
            }
            else if (msg.isNoteOff())
            {
                const int note = msg.getNoteNumber();
                for (int p = 0; p < kNumPads; ++p)
                {
                    // Only clear hold when the released note is the one the
                    // pad is currently rolling — otherwise a Keyboard-mode
                    // pad would unstick on an unrelated key.
                    auto& s = noteRepeat[(std::size_t) p];
                    if (s.held && s.heldNote == note)
                        s.held = false;
                }
            }
        }

        tickClocks (prev, numSamples);
    }

    bool ASamplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
    {
        // Main must be stereo (Voice::renderAdding writes to channels 0 and 1
        // unconditionally — supporting mono here would require routing logic
        // we don't otherwise need).
        if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
            return false;

        // Per-pad buses must be either disabled or stereo. The disabled state
        // is how REAPER reports "user has not opted into this bus", and we
        // route those pads back to Main.
        const int numOut = layouts.outputBuses.size();
        for (int i = 1; i < numOut && i <= kNumPads; ++i)
        {
            const auto& set = layouts.outputBuses.getReference (i);
            if (set != juce::AudioChannelSet::disabled()
                && set != juce::AudioChannelSet::stereo())
                return false;
        }

        return true;
    }

    VoiceLiveParams ASamplerProcessor::buildLiveParams (int padIndex) const
    {
        VoiceLiveParams live;
        const auto& p = padParams[(std::size_t) padIndex];

        live.gainLin         = juce::Decibels::decibelsToGain (p.volume->load(), -60.0f);
        live.pan             = p.pan->load();
        live.filterCutoffHz  = p.filterCutoff->load();
        live.filterResonance = p.filterResonance->load();
        live.satAmount       = p.satAmount->load();
        live.lfoRateHz       = p.lfoRate->load();
        live.lfoDepth        = p.lfoDepth->load();
        live.stereoWidth     = p.stereoWidth->load();
        live.monoBelowHz     = p.monoBelow->load();
        live.filterEnvOctaves = p.filtEnvAmount->load();
        return live;
    }

    int ASamplerProcessor::selectLayerForTrigger (int padIndex, int midiNote,
                                                  float velocity) const
    {
        // Enumerate user-loaded layers once for this trigger. The audio
        // thread's filePath read here races with message-thread loads, but the
        // worst case is a missed/spurious "is loaded" classification for one
        // trigger — no crash.
        int loadedLayers[kNumLayers];
        int loadedCount = 0;
        for (int L = 0; L < kNumLayers; ++L)
            if (! pads[padIndex].layers[(std::size_t) L].filePath.isEmpty())
                loadedLayers[loadedCount++] = L;

        // No user samples on this pad — layer 0 holds the default.
        if (loadedCount == 0)
            return 0;

        const auto mode = static_cast<LayerMode> (
                              juce::roundToInt (padParams[(std::size_t) padIndex].layerMode->load()));

        switch (mode)
        {
            case LayerMode::Velocity:
            {
                // Start at the band the velocity lands in, then search outward
                // (above first, then below within each distance) for a loaded
                // layer.
                const int chosen = layerForVelocity (velocity);
                if (! pads[padIndex].layers[(std::size_t) chosen].filePath.isEmpty())
                    return chosen;

                for (int d = 1; d < kNumLayers; ++d)
                {
                    if (chosen + d < kNumLayers
                        && ! pads[padIndex].layers[(std::size_t) (chosen + d)].filePath.isEmpty())
                        return chosen + d;

                    if (chosen - d >= 0
                        && ! pads[padIndex].layers[(std::size_t) (chosen - d)].filePath.isEmpty())
                        return chosen - d;
                }
                return 0;
            }

            case LayerMode::RoundRobin:
            {
                // Cycle through loaded layers in index order. Counter wraps via
                // modulo; reloading a layer mid-rotation is tolerated.
                const int next = roundRobinIndex[(std::size_t) padIndex]
                                     .fetch_add (1, std::memory_order_relaxed);
                return loadedLayers[((next % loadedCount) + loadedCount) % loadedCount];
            }

            case LayerMode::Random:
                return loadedLayers[juce::Random::getSystemRandom().nextInt (loadedCount)];

            case LayerMode::KeyZones:
            {
                // Pick the loaded layer whose [low, high] contains the note.
                // This is what turns four layers into a multisampled
                // instrument rather than one sample stretched across the range.
                for (int i = 0; i < loadedCount; ++i)
                {
                    const int L  = loadedLayers[i];
                    const int lo = juce::roundToInt (
                        padParams[(std::size_t) padIndex].layerLowNote[(std::size_t) L]->load());
                    const int hi = juce::roundToInt (
                        padParams[(std::size_t) padIndex].layerHighNote[(std::size_t) L]->load());

                    if (midiNote >= juce::jmin (lo, hi) && midiNote <= juce::jmax (lo, hi))
                        return L;
                }

                // Note falls outside every zone: use the loaded layer whose
                // root is closest, so the pad still speaks instead of going
                // silent on a key the user pressed.
                int best = loadedLayers[0];
                int bestDist = 1000;
                for (int i = 0; i < loadedCount; ++i)
                {
                    const int L = loadedLayers[i];
                    const int root = juce::roundToInt (
                        padParams[(std::size_t) padIndex].layerRootNote[(std::size_t) L]->load());
                    const int dist = std::abs (midiNote - root);
                    if (dist < bestDist) { bestDist = dist; best = L; }
                }
                return best;
            }
        }

        return 0;
    }

    void ASamplerProcessor::handleNoteOn (int midiNote, float velocity)
    {
        std::array<int, kNumPads> chokeTargets {};
        int chokeCount = 0;

        std::array<int, kNumPads> trigger {};
        int triggerCount = 0;

        for (int i = 0; i < kNumPads; ++i)
        {
            // Note match depends on the pad's dispatch mode. Fixed: only the
            // single assigned `Note` triggers. Keyboard: any incoming note in
            // [Low Note, High Note] triggers, and the pitch shift relative to
            // the root is applied in the trigger loop below.
            const auto padMode = static_cast<PadMode> (
                                     juce::roundToInt (padParams[i].mode->load()));
            bool matches = false;
            if (padMode == PadMode::Keyboard)
            {
                const int lo = juce::roundToInt (padParams[i].lowNote->load());
                const int hi = juce::roundToInt (padParams[i].highNote->load());
                matches = (midiNote >= lo && midiNote <= hi);
            }
            else
            {
                matches = (juce::roundToInt (padParams[i].note->load()) == midiNote);
            }
            if (! matches)
                continue;

            trigger[triggerCount++] = i;

            const int chokeGroup = juce::roundToInt (padParams[i].chokeGroup->load());
            if (chokeGroup <= 0)
                continue;

            for (int j = 0; j < kNumPads; ++j)
            {
                if (j == i) continue;
                const int otherGroup = juce::roundToInt (padParams[j].chokeGroup->load());
                if (otherGroup == chokeGroup)
                    chokeTargets[chokeCount++] = j;
            }
        }

        if (chokeCount > 0)
            voicePool.chokePads (chokeTargets.data(), chokeCount);

        for (int t = 0; t < triggerCount; ++t)
        {
            const int padIndex = trigger[t];

            // Layer selection happens first: in Key Zones mode the chosen
            // layer supplies the root note that the pitch offset is measured
            // from, so it has to be known before pitch is computed.
            const int selectedLayer = selectLayerForTrigger (padIndex, midiNote, velocity);

            VoiceConfig cfg;
            const float padGainDb = padParams[padIndex].volume->load();
            const auto  curve     = static_cast<VelocityCurve> (
                                        juce::roundToInt (padParams[padIndex].velocityCurve->load()));
            const float velGain   = applyVelocityCurve (velocity, curve);
            cfg.gainLin        = juce::Decibels::decibelsToGain (padGainDb, -60.0f) * velGain;
            cfg.hitGainScale   = velGain;   // so live volume pushes preserve this hit's velocity
            cfg.pan            = padParams[padIndex].pan->load();
            cfg.pitchSemitones = padParams[padIndex].pitch->load();

            // Keyboard mode: fold (incomingNote - rootNote) into the effective
            // pitch so the engine still gets a single semitone value. Done
            // before glide computation so lastTriggerPitchSt snapshots the
            // *effective* trigger pitch — sliding between keys then yields
            // the natural (currentNote - lastNote) semitone glide for free.
            const auto padMode = static_cast<PadMode> (
                                     juce::roundToInt (padParams[padIndex].mode->load()));
            const int  padRootNote = juce::roundToInt (padParams[padIndex].note->load());

            // In Key Zones mode each layer declares its own root, so the pitch
            // offset is measured from the sample that is actually playing —
            // that is what keeps a multisampled instrument in tune.
            const auto layerMode = static_cast<LayerMode> (
                                       juce::roundToInt (padParams[padIndex].layerMode->load()));
            const int rootNote = (layerMode == LayerMode::KeyZones)
                                   ? juce::roundToInt (padParams[padIndex]
                                         .layerRootNote[(std::size_t) selectedLayer]->load())
                                   : padRootNote;

            // Scale lock: snap the incoming note to the global key/scale before
            // deriving pitch, so playing a melodic chop across the keyboard
            // can't land off-key. Keyboard mode only — a fixed drum pad has no
            // incoming pitch to correct.
            int effectiveNote = midiNote;
            if (padMode == PadMode::Keyboard
                && padParams[padIndex].scaleLock->load() >= 0.5f
                && scaleKeyParam != nullptr && scaleTypeParam != nullptr)
            {
                effectiveNote = snapNoteToScale (
                    midiNote,
                    juce::roundToInt (scaleKeyParam->load()),
                    static_cast<Scale> (juce::roundToInt (scaleTypeParam->load())));
            }

            if (padMode == PadMode::Keyboard)
                cfg.pitchSemitones += static_cast<float> (effectiveNote - rootNote);

            cfg.startNorm      = padParams[padIndex].start->load();
            cfg.endNorm        = padParams[padIndex].end->load();
            cfg.reverse        = padParams[padIndex].reverse->load() >= 0.5f;
            cfg.filterMode     = static_cast<FilterMode> (
                                     juce::roundToInt (padParams[padIndex].filterMode->load()));
            cfg.filterCutoffHz = padParams[padIndex].filterCutoff->load();
            cfg.filterResonance = padParams[padIndex].filterResonance->load();
            cfg.loopMode       = static_cast<LoopMode> (
                                     juce::roundToInt (padParams[padIndex].loopMode->load()));
            cfg.loopStartNorm  = padParams[padIndex].loopStart->load();
            cfg.loopEndNorm    = padParams[padIndex].loopEnd->load();
            cfg.adsr.attack    = padParams[padIndex].attack->load();
            cfg.adsr.decay     = padParams[padIndex].decay->load();
            cfg.adsr.sustain   = padParams[padIndex].sustain->load();
            cfg.adsr.release   = padParams[padIndex].release->load();
            cfg.pitchBendSemitones = currentPitchBendSemitones;
            computeAftertouchMods (padIndex,
                                   cfg.aftertouchVolumeMultiplier,
                                   cfg.aftertouchCutoffMultiplier);
            cfg.padGate = resolvePadGate (padIndex);

            // Saturation / bit-crush character chain.
            cfg.satMode          = static_cast<SaturationMode> (
                                       juce::roundToInt (padParams[padIndex].satMode->load()));
            cfg.satAmount        = padParams[padIndex].satAmount->load();
            cfg.bitDepth         = juce::roundToInt (padParams[padIndex].bitDepth->load());
            cfg.downsampleFactor = juce::roundToInt (padParams[padIndex].downsample->load());

            // Pitch envelope.
            cfg.pitchEnvSemitones = padParams[padIndex].pitchEnvAmount->load();
            cfg.pitchEnvTimeMs    = padParams[padIndex].pitchEnvTime->load();

            // Per-pad LFO.
            cfg.lfoShape  = static_cast<LfoShape> (
                                juce::roundToInt (padParams[padIndex].lfoShape->load()));
            cfg.lfoDest   = static_cast<LfoDest> (
                                juce::roundToInt (padParams[padIndex].lfoDest->load()));
            cfg.lfoRateHz = padParams[padIndex].lfoRate->load();
            cfg.lfoDepth  = padParams[padIndex].lfoDepth->load();

            // Slice cleanup: edge fades, loop crossfade, zero-crossing snap.
            cfg.fadeInMs            = padParams[padIndex].fadeIn->load();
            cfg.fadeOutMs           = padParams[padIndex].fadeOut->load();
            cfg.loopXfadeMs         = padParams[padIndex].loopXfade->load();
            cfg.snapToZeroCrossings = padParams[padIndex].zeroSnap->load() >= 0.5f;

            // Velocity -> cutoff / start (velocity -> pitch is applied after
            // the glide snapshot below so it doesn't pollute the glide delta).
            const float vAmtCut   = padParams[padIndex].velToCutoff->load();
            const float vAmtStart = padParams[padIndex].velToStart->load();
            if (vAmtCut > 0.0f)
            {
                cfg.hitCutoffMultiplier = std::pow (2.0f, vAmtCut * velocity * 4.0f);
                cfg.filterCutoffHz     *= cfg.hitCutoffMultiplier;
            }
            if (vAmtStart > 0.0f)
                cfg.startNorm = juce::jlimit (0.0f, juce::jmax (0.0f, cfg.endNorm - 0.001f),
                                              cfg.startNorm + vAmtStart * velocity);

            // Glide / portamento. Mode gates whether to slide, time controls
            // how long. Each pad currently fires on one fixed MIDI note, so
            // "previous note" effectively means "previous trigger of this pad
            // with whatever pitch-param value was in force at that time".
            // First-ever trigger on a pad never glides — there is no source
            // pitch to come from.
            const auto glideMode = static_cast<GlideMode> (
                                       juce::roundToInt (padParams[padIndex].glideMode->load()));
            const float glideTimeMs   = padParams[padIndex].glideTime->load();
            const float currentPitch  = cfg.pitchSemitones;
            const int   lastNote      = lastTriggerNote[(std::size_t) padIndex];
            const float lastPitch     = lastTriggerPitchSt[(std::size_t) padIndex];

            const bool legatoOk = voicePool.hasActivePad (padIndex);
            const bool doGlide  = lastNote >= 0
                               && glideTimeMs > 0.0f
                               && glideMode != GlideMode::Off
                               && (! isLegatoGlide (glideMode) || legatoOk);

            if (doGlide)
            {
                const float deltaSemitones = lastPitch - currentPitch;
                cfg.glideStartSemitones = deltaSemitones;

                // Rate modes read Glide Time as milliseconds *per semitone*, so
                // one setting works for both a semitone step and an octave
                // jump. Fixed-time modes use it as the total duration, which
                // makes small intervals feel sluggish next to large ones.
                float effectiveTime = isRateGlide (glideMode)
                                        ? glideTimeMs * std::abs (deltaSemitones)
                                        : glideTimeMs;

                // Velocity -> glide time gives per-note control of slide length
                // from the piano roll: at full amount a hard hit snaps to pitch
                // and a soft hit takes the full time.
                const float vAmtGlide = padParams[padIndex].velToGlide->load();
                if (vAmtGlide > 0.0f)
                    effectiveTime *= juce::jmax (0.0f, 1.0f - vAmtGlide * velocity);

                // A rate glide over a huge interval could otherwise run for
                // minutes; cap it at the parameter's own maximum.
                cfg.glideTimeMs = juce::jlimit (0.0f, 2000.0f, effectiveTime);
            }
            else
            {
                cfg.glideStartSemitones = 0.0f;
                cfg.glideTimeMs         = 0.0f;
            }

            lastTriggerNote   [(std::size_t) padIndex] = midiNote;
            lastTriggerPitchSt[(std::size_t) padIndex] = currentPitch;

            // Velocity -> pitch, applied after the glide snapshot above so the
            // per-hit velocity offset doesn't pollute the note-to-note glide
            // delta (which is computed from lastTriggerPitchSt). Amount is in
            // semitones at full velocity.
            {
                const float vAmtPitch = padParams[padIndex].velToPitch->load();
                if (vAmtPitch != 0.0f)
                    cfg.pitchSemitones += vAmtPitch * velocity;
            }

            // Filter envelope.
            cfg.filterEnvOctaves       = padParams[padIndex].filtEnvAmount->load();
            cfg.filterEnvAdsr.attack   = padParams[padIndex].filtEnvAttack->load();
            cfg.filterEnvAdsr.decay    = padParams[padIndex].filtEnvDecay->load();
            cfg.filterEnvAdsr.sustain  = padParams[padIndex].filtEnvSustain->load();
            cfg.filterEnvAdsr.release  = padParams[padIndex].filtEnvRelease->load();

            // Stereo shaping.
            cfg.stereoWidth = padParams[padIndex].stereoWidth->load();
            cfg.monoBelowHz = padParams[padIndex].monoBelow->load();

            // Trigger note for the voice slot: in Fixed mode this is the
            // pad's assigned note (same as before — single-note dispatch);
            // in Keyboard mode it's the incoming MIDI note so a later
            // note-off can release just this voice via releasePadNote.
            //
            // Uses the raw incoming note, not the scale-snapped one, so a
            // note-off matches the note-on that the host actually sent.
            const int triggerNote = (padMode == PadMode::Keyboard) ? midiNote : rootNote;

            // Tempo sync. Scale the playback ratio by (hostBpm / originalBpm)
            // so a chopped loop lines up with the project. Expressed in
            // semitones because that's the engine's pitch currency — pitch
            // therefore moves with tempo, the classic repitch behaviour.
            //
            // Applied after the glide snapshot so a tempo change never reads
            // as a note-to-note glide.
            const auto stretchMode = static_cast<StretchMode> (
                                         juce::roundToInt (padParams[padIndex].stretchMode->load()));

            if (padParams[padIndex].tempoSync->load() >= 0.5f
                && stretchMode == StretchMode::Repitch)
            {
                const double origBpm = padParams[padIndex].originalBpm->load();
                if (origBpm > 0.0)
                {
                    const double ratio = getHostBpm() / origBpm;
                    if (ratio > 0.0)
                        cfg.pitchSemitones += static_cast<float> (
                            12.0 * std::log2 (ratio));
                }
            }

            // Snapshot the pad's current sample. atomic<shared_ptr> is C++20.
            // MSVC's implementation may use a brief internal lock; on Windows
            // for occasional sample swaps this is fine.
            // Per-layer gain trim. Folded into both the final gain and the
            // per-hit scale, so a live volume push preserves it the same way
            // it preserves velocity.
            {
                const float layerDb = padParams[padIndex]
                                          .layerGain[(std::size_t) selectedLayer]->load();
                if (layerDb != 0.0f)
                {
                    const float layerLin = juce::Decibels::decibelsToGain (layerDb, -60.0f);
                    cfg.gainLin      *= layerLin;
                    cfg.hitGainScale *= layerLin;
                }
            }

            // Sample selection, most specific first:
            //   1. the formant-corrected variant for this exact note offset,
            //   2. the pad's single stretched / formant-shifted render,
            //   3. the raw sample.
            // All three already carry the pad's settings, so playback stays a
            // plain resample either way.
            const auto& layerSlot = pads[padIndex].layers[(std::size_t) selectedLayer];
            std::shared_ptr<const Sample> sample;

            if (auto table = layerSlot.formantTable.load())
            {
                // cfg.pitchSemitones is the voice's whole pitch offset by this
                // point — note interval, Pitch parameter, tempo-sync repitch
                // and Vel->Pitch — so rounding it gives the correction that
                // matches everything at once. Residual error is at most half a
                // semitone of formant shift, which is inaudible.
                const int slotIndex = juce::roundToInt (cfg.pitchSemitones)
                                    + kMaxFormantOffset;

                if (slotIndex >= 0 && slotIndex < kFormantVariantSlots)
                    sample = table->variants[(std::size_t) slotIndex];
            }

            if (sample == nullptr)
                sample = layerSlot.processed.load();
            if (sample == nullptr)
                sample = layerSlot.sample.load();
            const int padCap = juce::roundToInt (padParams[padIndex].maxVoices->load());
            voicePool.triggerPad (padIndex, std::move (sample), triggerNote, cfg, padCap);
        }
    }

    void ASamplerProcessor::handlePitchBend (int wheelValue)
    {
        // MIDI pitch wheel is 14-bit: 0..16383, with 8192 = center (no bend).
        // Normalise to -1..+1 then multiply by the Pitch Bend Range param.
        const float normalised = (static_cast<float> (wheelValue) - 8192.0f) / 8192.0f;
        const int   rangeSt    = (pitchBendRangeParam != nullptr)
                                   ? juce::jlimit (1, 24, juce::roundToInt (pitchBendRangeParam->load()))
                                   : 2;
        currentPitchBendSemitones = normalised * static_cast<float> (rangeSt);
        voicePool.setPitchBendSemitones (currentPitchBendSemitones);
    }

    void ASamplerProcessor::handleAftertouch (int value)
    {
        currentAftertouch = juce::jlimit (0.0f, 1.0f,
                                          static_cast<float> (value) / 127.0f);

        // Recompute and push per-pad modulation: voices on a pad with
        // destination Off see (1.0, 1.0) which is a no-op.
        for (int p = 0; p < kNumPads; ++p)
        {
            float volMult = 1.0f, cutMult = 1.0f;
            computeAftertouchMods (p, volMult, cutMult);
            voicePool.setAftertouchModForPad (p, volMult, cutMult);
        }
    }

    void ASamplerProcessor::computeAftertouchMods (int padIndex,
                                                   float& outVolMult,
                                                   float& outCutoffMult) const
    {
        outVolMult    = 1.0f;
        outCutoffMult = 1.0f;
        if (padIndex < 0 || padIndex >= kNumPads) return;

        const auto& pp = padParams[(std::size_t) padIndex];
        if (pp.aftertouchDest == nullptr || pp.aftertouchAmount == nullptr) return;

        const auto dest   = static_cast<AftertouchDest> (
                                juce::roundToInt (pp.aftertouchDest->load()));
        const float amt   = juce::jlimit (0.0f, 1.0f, pp.aftertouchAmount->load());
        const float at    = juce::jlimit (0.0f, 1.0f, currentAftertouch);

        switch (dest)
        {
            case AftertouchDest::Off: break;

            case AftertouchDest::Volume:
                // Linear gain bump: amount=1 + AT=1 doubles output (+6 dB).
                outVolMult = 1.0f + amt * at;
                break;

            case AftertouchDest::FilterCutoff:
                // Exponential cutoff sweep: amount=1 + AT=1 multiplies cutoff
                // by 2^3 = 8x (~3 octaves up). Voice clamps to nyquist.
                outCutoffMult = std::pow (2.0f, amt * at * 3.0f);
                break;
        }
    }

    float ASamplerProcessor::resolvePadGate (int padIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return 1.0f;
        const auto& pp = padParams[(std::size_t) padIndex];
        if (pp.mute == nullptr || pp.solo == nullptr) return 1.0f;

        bool anySolo = false;
        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& other = padParams[(std::size_t) p];
            if (other.solo != nullptr && other.solo->load() >= 0.5f)
            {
                anySolo = true;
                break;
            }
        }

        if (anySolo)
            return pp.solo->load() >= 0.5f ? 1.0f : 0.0f;

        return pp.mute->load() >= 0.5f ? 0.0f : 1.0f;
    }

    void ASamplerProcessor::handleNoteOff (int midiNote)
    {
        for (int i = 0; i < kNumPads; ++i)
        {
            const auto padMode = static_cast<PadMode> (
                                     juce::roundToInt (padParams[i].mode->load()));

            bool matches = false;
            if (padMode == PadMode::Keyboard)
            {
                const int lo = juce::roundToInt (padParams[i].lowNote->load());
                const int hi = juce::roundToInt (padParams[i].highNote->load());
                matches = (midiNote >= lo && midiNote <= hi);
            }
            else
            {
                matches = (juce::roundToInt (padParams[i].note->load()) == midiNote);
            }
            if (! matches) continue;

            // One-shot pads ignore note-off — the voice runs to the end of
            // its sample window (or loop boundary). This matches the MPC
            // default for drum hits: a kick doesn't truncate when you lift.
            if (padParams[i].oneShot != nullptr
                && padParams[i].oneShot->load() >= 0.5f)
                continue;

            // In Keyboard mode a pad can hold simultaneous voices for several
            // incoming notes; release only the voices triggered by this note.
            // In Fixed mode the pad has at most one trigger note, so releasing
            // every voice on the pad is equivalent (and stays simple).
            if (padMode == PadMode::Keyboard)
                voicePool.releasePadNote (i, midiNote);
            else
                voicePool.releasePad (i);
        }
    }

    void ASamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midi)
    {
        juce::ScopedNoDenormals noDenormals;
        buffer.clear();

        // CC-bound parameter writes happen first so any value updates land
        // before the audio render reads them this block. Synthetic note
        // repeats won't see the writes (they read on the current block's
        // first event), but for sliders/knobs this is fine.
        handleMidiLearn (midi);

        // Push per-pad mute/solo state to every sounding voice so live
        // mute/solo changes apply mid-note (DAW-style track mute). New
        // voices triggered later in this block pick up the gate via cfg
        // in handleNoteOn.
        // ...and the live-modulatable parameter set, so automation and knob
        // moves are audible on notes that are already sounding (filter sweeps
        // over a held 808, volume rides on a long chop). The voice ramps the
        // click-prone values internally — see Voice::setLiveParams.
        for (int p = 0; p < kNumPads; ++p)
        {
            voicePool.setPadGate (p, resolvePadGate (p));
            voicePool.setPadLiveParams (p, buildLiveParams (p));
        }

        const int numSamples = buffer.getNumSamples();

        // Per-bus aliasing views into the host's combined output buffer.
        // busViews[0] = Main, busViews[i>0] = Pad i. AudioBuffer move-assignment
        // preserves the aliasing so we don't pay for a copy here.
        std::array<juce::AudioBuffer<float>, kNumPads + 1> busViews;
        for (int i = 0; i <= kNumPads; ++i)
            busViews[(std::size_t) i] = getBusBuffer (buffer, false, i);

        // Per-pad render destinations. A pad routes to its own bus if that
        // bus is enabled (has channels); otherwise its voices fold back into
        // Main. This is the "exclusive" convention — when an outboard bus is
        // enabled, the dry signal stops appearing in the main mix.
        std::array<juce::AudioBuffer<float>*, kNumPads> destByPad {};
        for (int p = 0; p < kNumPads; ++p)
        {
            auto* bus = getBus (false, p + 1);
            const bool padOutEnabled = (bus != nullptr && bus->isEnabled());
            destByPad[(std::size_t) p] = padOutEnabled ? &busViews[(std::size_t) (p + 1)]
                                                       : &busViews[0];
        }

        // Build a combined MIDI stream that interleaves the host's input with
        // any synthetic note-on repeats triggered by held pads with their
        // Repeat parameter on. The rest of the audio pipeline doesn't need to
        // know note repeat exists — it processes the combined buffer as if
        // every event came from the host.
        juce::MidiBuffer combinedMidi;
        buildCombinedMidi (midi, combinedMidi, numSamples);

        // Audition requests posted by the editor. Consumed here so voices are
        // only ever started on the audio thread. Exchange-to-clear means a
        // request fires exactly once even if two blocks race.
        for (int p = 0; p < kNumPads; ++p)
        {
            const int encoded = pendingAudition[(std::size_t) p]
                                    .exchange (0, std::memory_order_acq_rel);
            if (encoded <= 0)
                continue;

            const float velocity = static_cast<float> (encoded) / 1000.0f;
            const auto  padMode  = static_cast<PadMode> (
                                       juce::roundToInt (padParams[p].mode->load()));

            // Fire the note this pad actually listens for, so audition takes
            // exactly the same path as a real hit (choke groups, layers, glide).
            const int note = juce::roundToInt (padParams[p].note->load());
            juce::ignoreUnused (padMode);
            combinedMidi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), 0);

            // One-shot pads ignore note-off; the rest need one or the voice
            // would sustain forever. Release at the end of the block.
            if (padParams[p].oneShot->load() < 0.5f)
                combinedMidi.addEvent (juce::MidiMessage::noteOff (1, note),
                                       juce::jmax (0, numSamples - 1));
        }

        int currentSample = 0;
        for (const auto meta : combinedMidi)
        {
            const int eventSample = juce::jlimit (0, numSamples, meta.samplePosition);

            if (eventSample > currentSample)
            {
                voicePool.renderAdding (destByPad.data(), currentSample,
                                        eventSample - currentSample);
                currentSample = eventSample;
            }

            const auto msg = meta.getMessage();
            if (msg.isNoteOn())
                handleNoteOn (msg.getNoteNumber(), msg.getFloatVelocity());
            else if (msg.isNoteOff())
                handleNoteOff (msg.getNoteNumber());
            else if (msg.isPitchWheel())
                handlePitchBend (msg.getPitchWheelValue());
            else if (msg.isChannelPressure())
                handleAftertouch (msg.getChannelPressureValue());
            else if (msg.isAftertouch())
                // Polyphonic AT (0xA0). Forward as channel AT - we don't
                // currently track per-key pressure, but a poly-AT controller
                // should still produce some response rather than silence.
                handleAftertouch (msg.getAfterTouchValue());
        }

        if (currentSample < numSamples)
            voicePool.renderAdding (destByPad.data(), currentSample,
                                    numSamples - currentSample);

        // Master gain affects only Main. Per-pad outs are post-pad-fx but
        // pre-master, matching the standard MPC "send to external processing"
        // convention.
        const auto gainLin = juce::Decibels::decibelsToGain (masterGainParam->load(), -60.0f);
        busViews[0].applyGain (gainLin);
    }

    juce::AudioProcessorEditor* ASamplerProcessor::createEditor()
    {
        return new ASamplerEditor (*this);
    }

    const juce::String ASamplerProcessor::getName() const
    {
        return JucePlugin_Name;
    }

    // ---------------- Sample loading (message thread) ----------------

    ASamplerProcessor::LoadResult
    ASamplerProcessor::loadSampleForPad (int padIndex, const juce::File& file)
    {
        return loadSampleForPadLayer (padIndex, getCurrentEditLayer(), file);
    }

    ASamplerProcessor::LoadResult
    ASamplerProcessor::loadSampleForPadLayer (int padIndex, int layerIndex,
                                              const juce::File& file)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return LoadResult::FileNotFound;
        if (layerIndex < 0 || layerIndex >= kNumLayers)
            return LoadResult::FileNotFound;

        std::shared_ptr<const Sample> sample;
        const auto result = readSampleFile (file, sample);

        if (result != LoadResult::Success)
        {
            switch (result)
            {
                case LoadResult::FileNotFound:
                    setStatus ("File not found: " + file.getFileName()); break;
                case LoadResult::FormatNotSupported:
                    setStatus ("Unsupported audio format: " + file.getFileName()); break;
                case LoadResult::EmptyFile:
                    setStatus ("Empty audio file: " + file.getFileName()); break;
                case LoadResult::Success: break;
            }
            return result;
        }

        auto& slot = pads[padIndex].layers[(std::size_t) layerIndex];
        slot.sample.store (sample);
        slot.filePath = file.getFullPathName();
        slot.missing  = false;

        // New audio on this layer: drop any stretched/formant render of the
        // previous sample and reset the render keys so the timer re-renders.
        slot.processed.store (nullptr);
        slot.renderedTimeRatio = 1.0;
        slot.renderedFormant   = 0.0;
        slot.formantTable.store (nullptr);
        slot.tableKey = {};

        setStatus ("Loaded " + file.getFileName()
                   + " into Pad " + juce::String (padIndex + 1)
                   + " (Layer " + juce::String (layerIndex + 1) + ")");
        return LoadResult::Success;
    }

    ASamplerProcessor::LoadResult
    ASamplerProcessor::readSampleFile (const juce::File& file,
                                       std::shared_ptr<const Sample>& out) const
    {
        // Pure read — no pad state touched, no status posted. Split out so it
        // can run on the loader thread without racing message-thread state.
        if (! file.existsAsFile())
            return LoadResult::FileNotFound;

        std::unique_ptr<juce::AudioFormatReader> reader (
            const_cast<juce::AudioFormatManager&> (formatManager).createReaderFor (file));
        if (reader == nullptr)
            return LoadResult::FormatNotSupported;

        const int numChannels = juce::jmin (2, static_cast<int> (reader->numChannels));
        const int numSamples  = clampReaderLengthToInt (reader->lengthInSamples);

        if (numChannels <= 0 || numSamples <= 0)
            return LoadResult::EmptyFile;

        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        reader->read (&buffer, 0, numSamples, 0, true, numChannels >= 2);

        out = std::make_shared<const Sample> (std::move (buffer), reader->sampleRate);
        return LoadResult::Success;
    }

    void ASamplerProcessor::loadSampleForPadLayerAsync (int padIndex, int layerIndex,
                                                        const juce::File& file)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return;
        if (layerIndex < 0 || layerIndex >= kNumLayers) return;

        // Claim the slot's path straight away so the UI reflects the user's
        // intent immediately, then do the (potentially slow) disk read on the
        // loader thread. Announce both ends — a screen-reader user needs to
        // know a long load is in flight, not just that it finished.
        setStatus ("Loading " + file.getFileName()
                   + " into Pad " + juce::String (padIndex + 1) + "...");

        loaderPool.addJob ([this, padIndex, layerIndex, file]
        {
            std::shared_ptr<const Sample> sample;
            const auto result = readSampleFile (file, sample);

            // Hop back to the message thread to mutate pad state and announce,
            // so filePath stays single-threaded as the rest of the code assumes.
            juce::MessageManager::callAsync (
                [this, padIndex, layerIndex, file, result, sample]
                {
                    auto& slot = pads[padIndex].layers[(std::size_t) layerIndex];

                    if (result == LoadResult::Success)
                    {
                        slot.sample.store (sample);
                        slot.filePath = file.getFullPathName();
                        slot.missing  = false;
                        slot.processed.store (nullptr);
                        slot.renderedTimeRatio = 1.0;
                        slot.renderedFormant   = 0.0;
                        slot.formantTable.store (nullptr);
                        slot.tableKey = {};
                        setStatus ("Loaded " + file.getFileName()
                                   + " into Pad " + juce::String (padIndex + 1)
                                   + " (Layer " + juce::String (layerIndex + 1) + ")");
                    }
                    else
                    {
                        setStatus ("Could not load " + file.getFileName()
                                   + " into Pad " + juce::String (padIndex + 1));
                    }
                });
        });
    }

    int ASamplerProcessor::relinkMissingSamples (const juce::File& searchDir)
    {
        if (! searchDir.isDirectory())
        {
            setStatus ("Relink: not a folder");
            return 0;
        }

        // Index the folder by filename once, rather than walking it per missing
        // sample. Recursive so the user can point at the top of a sample
        // library and let it find things.
        std::map<juce::String, juce::File> byName;
        for (const auto& entry : juce::RangedDirectoryIterator (
                 searchDir, /*isRecursive*/ true, "*", juce::File::findFiles))
        {
            const auto f = entry.getFile();
            byName.emplace (f.getFileName().toLowerCase(), f);
        }

        int relinked = 0;
        int stillMissing = 0;

        for (int i = 0; i < kNumPads; ++i)
        {
            for (int L = 0; L < kNumLayers; ++L)
            {
                auto& slot = pads[i].layers[(std::size_t) L];
                if (! slot.missing || slot.filePath.isEmpty())
                    continue;

                const auto wanted = juce::File (slot.filePath).getFileName().toLowerCase();
                const auto found  = byName.find (wanted);
                if (found == byName.end())
                {
                    ++stillMissing;
                    continue;
                }

                if (loadSampleForPadLayer (i, L, found->second) == LoadResult::Success)
                    ++relinked;
                else
                    ++stillMissing;
            }
        }

        // One summary announcement rather than one per sample — a burst of
        // speech the user has to sit through is worse than no speech.
        setStatus ("Relinked " + juce::String (relinked) + " sample"
                   + (relinked == 1 ? "" : "s")
                   + (stillMissing > 0
                          ? (", " + juce::String (stillMissing) + " still missing")
                          : juce::String()));
        return relinked;
    }

    int ASamplerProcessor::getMissingSampleCount() const
    {
        int n = 0;
        for (const auto& pad : pads)
            for (const auto& layer : pad.layers)
                if (layer.missing)
                    ++n;
        return n;
    }

    bool ASamplerProcessor::normaliseLayer (int padIndex, int layerIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return false;
        if (layerIndex < 0 || layerIndex >= kNumLayers) return false;

        auto& slot = pads[padIndex].layers[(std::size_t) layerIndex];
        auto  src  = slot.sample.load();
        if (src == nullptr || src->isEmpty())
        {
            setStatus ("Nothing to normalise on Pad " + juce::String (padIndex + 1));
            return false;
        }

        // Find the peak, then set the *gain parameter* rather than rewriting
        // the audio. Non-destructive, automatable, undoable, and it reads out
        // as a dB value the user can then nudge by hand.
        float peak = 0.0f;
        for (int c = 0; c < src->getNumChannels(); ++c)
        {
            const float* d = src->getReadPointer (c);
            for (int i = 0; i < src->getNumSamples(); ++i)
                peak = juce::jmax (peak, std::abs (d[i]));
        }

        if (peak <= 0.0001f)
        {
            setStatus ("Pad " + juce::String (padIndex + 1) + " layer is silent");
            return false;
        }

        // Target -1 dBFS so a normalised layer still has a little headroom.
        const float targetDb = -1.0f;
        const float gainDb   = targetDb - juce::Decibels::gainToDecibels (peak);
        setRangedParam (param_ids::padLayerGain (padIndex + 1, layerIndex + 1),
                        juce::jlimit (-24.0f, 24.0f, gainDb));

        setStatus ("Normalised Pad " + juce::String (padIndex + 1)
                   + " Layer " + juce::String (layerIndex + 1)
                   + " to " + juce::String (gainDb, 1) + " dB");
        return true;
    }

    void ASamplerProcessor::resetPadToDefault (int padIndex)
    {
        resetPadLayerToDefault (padIndex, getCurrentEditLayer());
    }

    void ASamplerProcessor::resetPadLayerToDefault (int padIndex, int layerIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return;
        if (layerIndex < 0 || layerIndex >= kNumLayers) return;

        auto& slot = pads[padIndex].layers[(std::size_t) layerIndex];
        slot.sample.store (defaultSample);
        slot.filePath = {};
        slot.processed.store (nullptr);
        slot.renderedTimeRatio = 1.0;
        slot.renderedFormant   = 0.0;
        slot.formantTable.store (nullptr);
        slot.tableKey = {};
        setStatus ("Reset Pad " + juce::String (padIndex + 1)
                   + " (Layer " + juce::String (layerIndex + 1) + ") to default");
    }

    int ASamplerProcessor::getCurrentChopCount() const
    {
        if (chopCountParam == nullptr) return chopCountFromIndex (3);
        return chopCountFromIndex (juce::roundToInt (chopCountParam->load()));
    }

    int ASamplerProcessor::getCurrentEditLayer() const
    {
        if (editLayerParam == nullptr) return 0;
        return juce::jlimit (0, kNumLayers - 1,
                             juce::roundToInt (editLayerParam->load()));
    }

    int ASamplerProcessor::getCurrentVocalTargetPad() const
    {
        if (vocalTargetPadParam == nullptr) return 0;
        // Parameter is 1-based ("Pad 1".."Pad 16"); expose a 0-based index.
        return juce::jlimit (0, kNumPads - 1,
                             juce::roundToInt (vocalTargetPadParam->load()) - 1);
    }

    void ASamplerProcessor::setRangedParam (const juce::String& paramId, float plainValue)
    {
        auto* p = state.getParameter (paramId);
        if (p == nullptr)
            return;

        const float clamped = p->getNormalisableRange().snapToLegalValue (plainValue);

        // Write through the APVTS ValueTree rather than straight to the
        // parameter object. The tree write is what the UndoManager records, and
        // APVTS propagates it back to the parameter (and on to the host) via
        // setValueNotifyingHost. Writing the parameter directly instead would
        // only reach the tree later, on APVTS's internal timer — so a batch
        // action like auto-chop would not land inside the transaction we opened
        // for it, and would not be undoable as one step.
        if (auto value = state.getParameterAsValue (paramId); ! value.getValue().isVoid())
        {
            value.setValue (clamped);
            return;
        }

        // No tree node yet (shouldn't happen after construction) — fall back so
        // the value still takes effect, just without undo.
        p->setValueNotifyingHost (p->convertTo0to1 (clamped));
    }

    ASamplerProcessor::VocalResult
    ASamplerProcessor::synthesizeVocalForPad (int padIndex, const juce::String& text)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
        {
            setStatus ("Invalid pad for vocal generation");
            return VocalResult::InvalidPad;
        }

        const auto trimmed = text.trim();
        if (trimmed.isEmpty())
        {
            setStatus ("Type some text before generating a vocal");
            return VocalResult::EmptyText;
        }

        // Durable, per-user location so the generated sample survives a project
        // reload (the pad persists its file path, same as a user-loaded sample).
        const juce::File dir =
            juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                .getChildFile ("ASampler")
                .getChildFile ("vocals");
        dir.createDirectory();

        const juce::String slug  = juce::File::createLegalFileName (trimmed.substring (0, 24)).trim();
        const juce::String stamp = juce::String (juce::Time::currentTimeMillis());
        const juce::File   wav   = dir.getChildFile (
            "vocal_" + (slug.isEmpty() ? juce::String ("clip") : slug) + "_" + stamp + ".wav");

        const auto synth = synthesizeSpeechToWav (trimmed, wav);
        if (! synth.ok)
        {
            setStatus ("Vocal generation failed: " + synth.error);
            return VocalResult::SynthFailed;
        }

        if (loadSampleForPad (padIndex, wav) != LoadResult::Success)
        {
            setStatus ("Vocal was spoken but the audio could not be loaded");
            return VocalResult::LoadFailed;
        }

        // loadSampleForPad set its own "Loaded ..." status; replace it with a
        // vocal-specific one that a screen reader can read back meaningfully.
        setStatus ("Spoke \"" + trimmed + "\" into Pad " + juce::String (padIndex + 1));
        return VocalResult::Success;
    }

    ASamplerProcessor::VocalResult
    ASamplerProcessor::synthesizeVocalAndChop (int padIndex, const juce::String& text)
    {
        const auto r = synthesizeVocalForPad (padIndex, text);
        if (r != VocalResult::Success)
            return r;

        const auto chop = chopAcrossPads (padIndex);
        if (chop == ChopResult::Success)
            setStatus ("Chopped vocal across " + juce::String (getCurrentChopCount())
                       + " pads from Pad " + juce::String (padIndex + 1));
        else
            setStatus ("Vocal loaded on Pad " + juce::String (padIndex + 1)
                       + ", but the chop step failed");
        return r;
    }

    ASamplerProcessor::VocalResult
    ASamplerProcessor::synthesizeVocalAndSpread (int padIndex, const juce::String& text,
                                                 int lowNote, int highNote)
    {
        const auto r = synthesizeVocalForPad (padIndex, text);
        if (r != VocalResult::Success)
            return r;

        const int lo = juce::jlimit (0, 127, juce::jmin (lowNote, highNote));
        const int hi = juce::jlimit (0, 127, juce::jmax (lowNote, highNote));
        const int p  = padIndex + 1;   // param ids are 1-based

        // Keyboard mode with the root at the low note: the lowest key plays the
        // vocal at its natural pitch and higher keys pitch it up.
        setRangedParam (param_ids::padMode     (p), static_cast<float> (static_cast<int> (PadMode::Keyboard)));
        setRangedParam (param_ids::padNote     (p), static_cast<float> (lo));
        setRangedParam (param_ids::padLowNote  (p), static_cast<float> (lo));
        setRangedParam (param_ids::padHighNote (p), static_cast<float> (hi));

        setStatus ("Spread vocal across keys " + juce::String (lo) + " to "
                   + juce::String (hi) + " on Pad " + juce::String (p));
        return r;
    }

    ChopMode ASamplerProcessor::getCurrentChopMode() const
    {
        if (chopModeParam == nullptr) return ChopMode::Equal;
        const int idx = juce::roundToInt (chopModeParam->load());
        switch (idx)
        {
            case static_cast<int> (ChopMode::Transients):         return ChopMode::Transients;
            case static_cast<int> (ChopMode::TransientsSpectral): return ChopMode::TransientsSpectral;
            default:                                              return ChopMode::Equal;
        }
    }

    ASamplerProcessor::ChopResult
    ASamplerProcessor::chopAcrossPads (int sourcePadIndex)
    {
        return chopAcrossPads (sourcePadIndex, getCurrentChopCount(), getCurrentChopMode());
    }

    ASamplerProcessor::ChopResult
    ASamplerProcessor::chopAcrossPads (int sourcePadIndex, int numSlices)
    {
        return chopAcrossPads (sourcePadIndex, numSlices, ChopMode::Equal);
    }

    ASamplerProcessor::ChopResult
    ASamplerProcessor::chopAcrossPads (int sourcePadIndex, int numSlices, ChopMode mode)
    {
        if (sourcePadIndex < 0 || sourcePadIndex >= kNumPads)
            return ChopResult::InvalidSourcePad;
        if (numSlices < 1 || numSlices > kNumPads)
            return ChopResult::InvalidSliceCount;

        // Chop reads from and writes to layer 0 only. Chops are single-sample-
        // per-pad workflows; velocity layers above layer 0 aren't touched, so a
        // user who has carefully built up layered drums can chop a break onto
        // pad 1's layer 0 without nuking the higher layers.
        constexpr int kChopLayer = 0;
        auto& sourceLayer = pads[sourcePadIndex].layers[kChopLayer];
        if (sourceLayer.filePath.isEmpty())
        {
            setStatus ("Cannot chop: Pad " + juce::String (sourcePadIndex + 1)
                       + " has no loaded sample on layer 1");
            return ChopResult::SourcePadEmpty;
        }

        // Everything below rewrites the start/end window (and sample) on up to
        // 16 pads. Group it into one undo transaction so a single Undo puts all
        // of them back — this is the most destructive action in the plugin and
        // the hardest to notice without sight.
        beginUndoTransaction (
            "Chop Pad " + juce::String (sourcePadIndex + 1)
            + " across " + juce::String (numSlices) + " pads");

        // Snapshot the source — capturing the shared_ptr before we start writing
        // means we're safe even if the source pad is also one of the destinations
        // and gets its sample atomic overwritten partway through.
        auto sourceSample   = sourceLayer.sample.load();
        auto sourceFilePath = sourceLayer.filePath;

        // Build per-slice [start, end] normalised boundaries. In Equal mode
        // each slice is 1/N wide; in Transients mode we run onset detection
        // on the source sample and use the top-N hits as slice starts. If
        // we can't find enough hits, we fall back to Equal so the chop still
        // produces a usable result (with a status message saying so).
        std::vector<float> startNorms (static_cast<std::size_t> (numSlices));
        std::vector<float> endNorms   (static_cast<std::size_t> (numSlices));

        const float step          = 1.0f / static_cast<float> (numSlices);
        bool        usedTransients = false;
        int         hitsFound      = 0;

        const bool wantsTransients = (mode == ChopMode::Transients
                                       || mode == ChopMode::TransientsSpectral);

        if (wantsTransients && sourceSample != nullptr && ! sourceSample->isEmpty())
        {
            // Copy the Sample's audio into an AudioBuffer the detector can
            // read. A copy keeps the const-correctness of Sample::getReadPointer
            // intact; for chop (a one-shot message-thread action) the extra
            // alloc is negligible.
            const int numCh = sourceSample->getNumChannels();
            const int numS  = sourceSample->getNumSamples();
            juce::AudioBuffer<float> audioCopy (numCh, numS);
            for (int ch = 0; ch < numCh; ++ch)
                audioCopy.copyFrom (ch, 0, sourceSample->getReadPointer (ch), numS);

            const auto onsetMethod = (mode == ChopMode::TransientsSpectral)
                                       ? OnsetMethod::SpectralFlux
                                       : OnsetMethod::LogEnergy;
            const auto hits = detectTransients (audioCopy,
                                                sourceSample->getSourceSampleRate(),
                                                onsetMethod);
            hitsFound = static_cast<int> (hits.size());

            if (hitsFound >= numSlices)
            {
                // Take the top-N strongest, then sort back into time order so
                // they form a sequence of monotonic slice boundaries.
                auto sorted = hits;
                std::partial_sort (sorted.begin(), sorted.begin() + numSlices, sorted.end(),
                    [] (const TransientHit& a, const TransientHit& b)
                    { return a.strength > b.strength; });
                sorted.resize (static_cast<std::size_t> (numSlices));
                std::sort (sorted.begin(), sorted.end(),
                    [] (const TransientHit& a, const TransientHit& b)
                    { return a.sampleIndex < b.sampleIndex; });

                const float lengthMinusOne = static_cast<float> (juce::jmax (1, numS - 1));
                for (int i = 0; i < numSlices; ++i)
                {
                    startNorms[(std::size_t) i] = static_cast<float> (sorted[(std::size_t) i].sampleIndex)
                                                / lengthMinusOne;
                    endNorms  [(std::size_t) i] = (i + 1 < numSlices)
                        ? static_cast<float> (sorted[(std::size_t) (i + 1)].sampleIndex) / lengthMinusOne
                        : 1.0f;
                }
                usedTransients = true;
            }
        }

        if (! usedTransients)
        {
            for (int i = 0; i < numSlices; ++i)
            {
                startNorms[(std::size_t) i] = step * static_cast<float> (i);
                endNorms  [(std::size_t) i] = (i + 1 == numSlices) ? 1.0f
                                                                   : step * static_cast<float> (i + 1);
            }
        }

        for (int i = 0; i < numSlices; ++i)
        {
            // Sample handoff: every destination pad's layer 0 shares the same
            // Sample. The voice copies its own shared_ptr at trigger time, so a
            // later sample swap on any of these pads is safe. Higher layers on
            // destination pads are intentionally left untouched.
            auto& destLayer = pads[i].layers[kChopLayer];
            destLayer.sample.store (sourceSample);
            destLayer.filePath = sourceFilePath;

            // Via setRangedParam so these land in the undo transaction opened
            // above — writing the parameter object directly would not.
            setRangedParam (param_ids::padStart (i + 1), startNorms[(std::size_t) i]);
            setRangedParam (param_ids::padEnd   (i + 1), endNorms  [(std::size_t) i]);
        }

        // Status messages spell out what actually happened so the screen
        // reader announces the real outcome (especially the transient-fallback
        // case, where the user asked for one thing and got another).
        juce::String how;
        const char* transientLabel = (mode == ChopMode::TransientsSpectral)
                                       ? "transients (spectral)"
                                       : "transients";
        if (wantsTransients && usedTransients)
            how = transientLabel;
        else if (wantsTransients)
            how = "equal (only " + juce::String (hitsFound) + " "
                + transientLabel + " found)";
        else
            how = "equal";

        setStatus ("Chopped Pad " + juce::String (sourcePadIndex + 1)
                   + " sample into " + juce::String (numSlices)
                   + " slices across pads 1-" + juce::String (numSlices)
                   + " using " + how
                   + ". Undo restores the previous pad settings");
        return ChopResult::Success;
    }

    juce::String ASamplerProcessor::getPadSampleDisplayName (int padIndex) const
    {
        return getPadLayerSampleDisplayName (padIndex, getCurrentEditLayer());
    }

    juce::String
    ASamplerProcessor::getPadLayerSampleDisplayName (int padIndex, int layerIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return {};
        if (layerIndex < 0 || layerIndex >= kNumLayers) return {};

        const auto& layer = pads[padIndex].layers[(std::size_t) layerIndex];
        if (layer.filePath.isEmpty())
            return "default";

        const auto name = juce::File (layer.filePath).getFileName();

        // Say so out loud rather than quietly reading as if the sample loaded.
        return layer.missing ? (name + " (missing)") : name;
    }

    juce::String ASamplerProcessor::getLastStatusMessage() const
    {
        return lastStatus;
    }

    int ASamplerProcessor::clampReaderLengthToInt (juce::int64 lengthInSamples) noexcept
    {
        if (lengthInSamples <= 0) return 0;
        constexpr juce::int64 kMaxInt = static_cast<juce::int64> (std::numeric_limits<int>::max());
        return static_cast<int> (juce::jmin (lengthInSamples, kMaxInt));
    }

    // ---------------- MIDI learn ----------------

    void ASamplerProcessor::bindCcToParameter (int cc, const juce::String& paramId)
    {
        if (cc < 0 || cc > 127) return;

        if (paramId.isEmpty())
        {
            ccBindings[(std::size_t) cc].store (-1, std::memory_order_relaxed);
            setStatus ("CC " + juce::String (cc) + " unbound");
            sendChangeMessage();
            return;
        }

        const auto& params = getParameters();
        for (int i = 0; i < params.size(); ++i)
        {
            if (auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (params[i]))
            {
                if (withId->paramID == paramId)
                {
                    ccBindings[(std::size_t) cc].store (i, std::memory_order_relaxed);
                    setStatus ("Bound CC " + juce::String (cc) + " to " + withId->name);
                    sendChangeMessage();
                    return;
                }
            }
        }
        // No match — silently leave the slot unbound; saves with stale IDs
        // (renamed params, old project files) don't blow up.
        ccBindings[(std::size_t) cc].store (-1, std::memory_order_relaxed);
    }

    void ASamplerProcessor::unbindCc (int cc)
    {
        if (cc < 0 || cc > 127) return;
        ccBindings[(std::size_t) cc].store (-1, std::memory_order_relaxed);
        sendChangeMessage();
    }

    juce::String ASamplerProcessor::getCcBinding (int cc) const
    {
        if (cc < 0 || cc > 127) return {};
        const int idx = ccBindings[(std::size_t) cc].load (std::memory_order_relaxed);
        if (idx < 0) return {};
        const auto& params = getParameters();
        if (idx >= params.size()) return {};
        if (auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (params[idx]))
            return withId->paramID;
        return {};
    }

    int ASamplerProcessor::getLastTouchedParameterIndex() const
    {
        return lastTouchedParameterIndex.load (std::memory_order_relaxed);
    }

    void ASamplerProcessor::parameterValueChanged (int parameterIndex, float)
    {
        // Value changes flow through automatically — we only care about
        // gestures, since "the user touched parameter X" is what powers the
        // learn workflow.
        //
        // The one exception is "Focused Pad": every Focus proxy now reads a
        // different underlying parameter, so they all have to tell the host.
        // Without this the FX list keeps showing (and the screen reader keeps
        // speaking) the previously focused pad's values.
        if (parameterIndex == focusedPadParamIndex)
        {
            for (auto* proxy : focusProxies)
                if (proxy != nullptr)
                    proxy->refreshFromFocus();

            if (focusedPadParam != nullptr)
                setStatus ("Focus: Pad "
                           + juce::String (juce::roundToInt (focusedPadParam->load())));
        }
    }

    void ASamplerProcessor::parameterGestureChanged (int parameterIndex,
                                                     bool gestureIsStarting)
    {
        if (! gestureIsStarting) return;
        if (dispatchingMidiLearn.load (std::memory_order_relaxed)) return;

        // Don't let arming the learn parameter itself count as "the user
        // touched parameter X". Otherwise turning the arm on would set the
        // learn target to the arm param, which is meaningless.
        const auto& params = getParameters();
        if (parameterIndex >= 0 && parameterIndex < params.size())
        {
            if (auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (
                                   params[parameterIndex]))
            {
                if (withId->paramID == param_ids::midiLearnArm)
                    return;
            }
        }

        lastTouchedParameterIndex.store (parameterIndex, std::memory_order_relaxed);
    }

    void ASamplerProcessor::handleMidiLearn (const juce::MidiBuffer& input)
    {
        const auto& params = getParameters();

        for (const auto meta : input)
        {
            const auto msg = meta.getMessage();
            if (! msg.isController()) continue;

            const int cc    = msg.getControllerNumber();
            const int value = msg.getControllerValue();    // 0..127
            if (cc < 0 || cc > 127) continue;

            // Arm: bind this CC to the most recent user-touched parameter
            // (if any), then auto-disarm so the next CC isn't captured too.
            const bool armed = (midiLearnArmParam != nullptr)
                                 && (midiLearnArmParam->load() >= 0.5f);
            if (armed)
            {
                const int target = lastTouchedParameterIndex.load (std::memory_order_relaxed);
                if (target >= 0 && target < params.size())
                {
                    if (auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (
                                           params[target]))
                    {
                        ccBindings[(std::size_t) cc].store (target, std::memory_order_relaxed);
                        setStatus ("Bound CC " + juce::String (cc) + " to " + withId->name);
                    }
                }

                // Disarm regardless of whether we managed to find a target —
                // a single arm captures at most one CC. Suppress gesture
                // re-entry so this write doesn't reset lastTouched.
                if (auto* armParam = state.getParameter (param_ids::midiLearnArm))
                {
                    dispatchingMidiLearn.store (true, std::memory_order_relaxed);
                    armParam->setValueNotifyingHost (0.0f);
                    dispatchingMidiLearn.store (false, std::memory_order_relaxed);
                }
            }

            const int boundIndex = ccBindings[(std::size_t) cc].load (std::memory_order_relaxed);
            if (boundIndex >= 0 && boundIndex < params.size())
            {
                const float normalised = static_cast<float> (value) / 127.0f;
                dispatchingMidiLearn.store (true, std::memory_order_relaxed);
                params[boundIndex]->setValueNotifyingHost (normalised);
                dispatchingMidiLearn.store (false, std::memory_order_relaxed);
            }
        }
    }

    // ---------------- Undo ----------------

    void ASamplerProcessor::materialiseParameterValueProperties()
    {
        // AudioProcessorValueTreeState gives each parameter a child node
        // carrying only its "id"; the "value" property is created lazily by the
        // first flush. That first write passes a null UndoManager (see
        // ParameterAdapter::flushToTree), so the very first change to any
        // parameter is silently *not* undoable — including the first chop a
        // user performs, which is exactly the one worth being able to undo.
        //
        // Creating the properties up front makes every subsequent write take
        // the undoable branch.
        auto& tree = state.state;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild (i);
            if (! child.hasType ("PARAM") || child.hasProperty ("value"))
                continue;

            const auto id = child.getProperty ("id").toString();
            if (auto* param = state.getParameter (id))
                child.setProperty ("value", param->convertFrom0to1 (param->getValue()), nullptr);
        }
    }

    void ASamplerProcessor::beginUndoTransaction (const juce::String& name)
    {
        undoManager.beginNewTransaction (name);
    }

    bool ASamplerProcessor::undoLastAction()
    {
        const bool ok = undoManager.undo();
        setStatus (ok ? "Undo" : "Nothing to undo");
        return ok;
    }

    bool ASamplerProcessor::redoLastAction()
    {
        const bool ok = undoManager.redo();
        setStatus (ok ? "Redo" : "Nothing to redo");
        return ok;
    }

    // ---------------- Kits ----------------

    bool ASamplerProcessor::saveKitToFile (const juce::File& file)
    {
        auto root = juce::ValueTree ("ASamplerKit");
        root.setProperty ("version", 1, nullptr);
        root.addChild (state.copyState().createCopy(), -1, nullptr);

        auto samples = juce::ValueTree ("samples");
        for (int i = 0; i < kNumPads; ++i)
        {
            for (int L = 0; L < kNumLayers; ++L)
            {
                const auto& path = pads[i].layers[(std::size_t) L].filePath;
                if (path.isEmpty()) continue;

                auto entry = juce::ValueTree ("pad");
                entry.setProperty ("index", i, nullptr);
                entry.setProperty ("layer", L, nullptr);
                entry.setProperty ("path",  path, nullptr);
                samples.appendChild (entry, nullptr);
            }
        }
        root.addChild (samples, -1, nullptr);

        auto xml = root.createXml();
        if (xml == nullptr || ! xml->writeTo (file))
        {
            setStatus ("Could not save kit to " + file.getFileName());
            return false;
        }

        setStatus ("Saved kit " + file.getFileNameWithoutExtension());
        return true;
    }

    bool ASamplerProcessor::loadKitFromFile (const juce::File& file)
    {
        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr)
        {
            setStatus ("Could not read kit " + file.getFileName());
            return false;
        }

        auto root = juce::ValueTree::fromXml (*xml);
        if (! root.isValid() || ! root.hasType ("ASamplerKit"))
        {
            setStatus (file.getFileName() + " is not an ASampler kit");
            return false;
        }

        beginUndoTransaction ("Load kit " + file.getFileNameWithoutExtension());

        auto params = root.getChildWithName (state.state.getType());
        if (params.isValid())
            state.replaceState (params.createCopy());

        for (auto& pad : pads)
        {
            for (auto& layer : pad.layers)
            {
                layer.sample.store (defaultSample);
                layer.filePath = {};
                layer.missing  = false;
                layer.processed.store (nullptr);
                layer.renderedTimeRatio = 1.0;
                layer.renderedFormant   = 0.0;
                layer.formantTable.store (nullptr);
                layer.tableKey = {};
            }
        }

        int restored = 0;
        int missing  = 0;
        auto samples = root.getChildWithName ("samples");
        for (int i = 0; i < samples.getNumChildren(); ++i)
        {
            auto entry = samples.getChild (i);
            const int  idx   = entry.getProperty ("index", -1);
            const int  layer = entry.getProperty ("layer", 0);
            const auto path  = entry.getProperty ("path", "").toString();

            if (idx < 0 || idx >= kNumPads || layer < 0 || layer >= kNumLayers || path.isEmpty())
                continue;

            std::shared_ptr<const Sample> sample;
            auto& slot = pads[idx].layers[(std::size_t) layer];
            if (readSampleFile (juce::File (path), sample) == LoadResult::Success)
            {
                slot.sample.store (sample);
                slot.filePath = path;
                ++restored;
            }
            else
            {
                slot.filePath = path;
                slot.missing  = true;
                ++missing;
            }
        }

        setStatus ("Loaded kit " + file.getFileNameWithoutExtension()
                   + " - " + juce::String (restored) + " samples"
                   + (missing > 0 ? (", " + juce::String (missing) + " missing") : juce::String()));
        return true;
    }

    juce::StringArray ASamplerProcessor::getFactoryKitNames()
    {
        return { "Trap", "Boom Bap", "Lo-Fi", "808 Set" };
    }

    bool ASamplerProcessor::loadFactoryKit (int index)
    {
        const auto names = getFactoryKitNames();
        if (index < 0 || index >= names.size())
        {
            setStatus ("Unknown factory kit");
            return false;
        }

        beginUndoTransaction ("Load kit " + names[index]);

        // Factory kits configure the *setup* — envelopes, choke groups, note
        // layout, character — and leave samples to the user. That is the part
        // that is slow to rebuild by hand, and it makes the kits useful without
        // shipping any audio.
        //
        // Pads 1-16 keep their default note layout; we only change sound and
        // voicing. Pad 2/3 are treated as the hat pair (choke group 1), the
        // usual MPC convention.
        for (int p = 1; p <= kNumPads; ++p)
        {
            // Shared baseline: snappy one-shot drums, no loop, clean.
            setRangedParam (param_ids::padAttack (p),   0.001f);
            setRangedParam (param_ids::padSustain (p),  1.0f);
            setRangedParam (param_ids::padOneShot (p),  1.0f);
            setRangedParam (param_ids::padLoopMode (p), 0.0f);
            setRangedParam (param_ids::padSatAmount (p), 0.0f);
            setRangedParam (param_ids::padBitDepth (p),  16.0f);
            setRangedParam (param_ids::padDownsample (p), 1.0f);
            setRangedParam (param_ids::padTempoSync (p),  0.0f);
            setRangedParam (param_ids::padFadeIn (p),  0.5f);
            setRangedParam (param_ids::padFadeOut (p), 2.0f);
            setRangedParam (param_ids::padZeroSnap (p), 1.0f);

            // Closed/open hat pair chokes, so an open hat is cut by a closed
            // one — the single most important default in any hip-hop kit.
            setRangedParam (param_ids::padChokeGroup (p),
                            (p == 2 || p == 3) ? 1.0f : 0.0f);
        }

        switch (index)
        {
            case 0:  // Trap — tight, bright, fast hats
                for (int p = 1; p <= kNumPads; ++p)
                {
                    setRangedParam (param_ids::padRelease (p), 0.08f);
                    setRangedParam (param_ids::padDecay (p),   0.25f);
                    setRangedParam (param_ids::padRepeatRate (p),
                                    static_cast<float> (static_cast<int> (NoteRepeatRate::ThirtySecond)));
                }
                // 808 on pad 1: long, keyboard-playable, with pitch punch.
                setRangedParam (param_ids::padMode (1),
                                static_cast<float> (static_cast<int> (PadMode::Keyboard)));
                setRangedParam (param_ids::padLowNote (1),  24.0f);
                setRangedParam (param_ids::padHighNote (1), 60.0f);
                setRangedParam (param_ids::padRelease (1),  0.6f);
                setRangedParam (param_ids::padGlideMode (1),
                                static_cast<float> (static_cast<int> (GlideMode::Legato)));
                setRangedParam (param_ids::padGlideTime (1), 60.0f);
                setRangedParam (param_ids::padMaxVoices (1), 1.0f);
                setRangedParam (param_ids::padPitchEnvAmount (1), 4.0f);
                setRangedParam (param_ids::padPitchEnvTime (1),  40.0f);
                break;

            case 1:  // Boom Bap — rounder, softer top, a little tube warmth
                for (int p = 1; p <= kNumPads; ++p)
                {
                    setRangedParam (param_ids::padRelease (p), 0.15f);
                    setRangedParam (param_ids::padSatMode (p),
                                    static_cast<float> (static_cast<int> (SaturationMode::Tube)));
                    setRangedParam (param_ids::padSatAmount (p), 0.2f);
                    setRangedParam (param_ids::padFilterMode (p),
                                    static_cast<float> (static_cast<int> (FilterMode::LowPass)));
                    setRangedParam (param_ids::padFilterCutoff (p), 12000.0f);
                    setRangedParam (param_ids::padRepeatRate (p),
                                    static_cast<float> (static_cast<int> (NoteRepeatRate::Sixteenth)));
                    setRangedParam (param_ids::padSwing (p), 0.12f);
                    setRangedParam (param_ids::padHumanize (p), 0.15f);
                }
                break;

            case 2:  // Lo-Fi — tape saturation, bit + rate reduction, dark
                for (int p = 1; p <= kNumPads; ++p)
                {
                    setRangedParam (param_ids::padRelease (p), 0.2f);
                    setRangedParam (param_ids::padSatMode (p),
                                    static_cast<float> (static_cast<int> (SaturationMode::Tape)));
                    setRangedParam (param_ids::padSatAmount (p), 0.35f);
                    setRangedParam (param_ids::padBitDepth (p), 12.0f);
                    setRangedParam (param_ids::padDownsample (p), 2.0f);
                    setRangedParam (param_ids::padFilterMode (p),
                                    static_cast<float> (static_cast<int> (FilterMode::LowPass)));
                    setRangedParam (param_ids::padFilterCutoff (p), 7000.0f);
                    setRangedParam (param_ids::padSwing (p), 0.18f);
                    setRangedParam (param_ids::padHumanize (p), 0.25f);
                }
                break;

            case 3:  // 808 Set — every pad a playable, gliding sub
                for (int p = 1; p <= kNumPads; ++p)
                {
                    setRangedParam (param_ids::padMode (p),
                                    static_cast<float> (static_cast<int> (PadMode::Keyboard)));
                    setRangedParam (param_ids::padLowNote (p),  24.0f);
                    setRangedParam (param_ids::padHighNote (p), 60.0f);
                    setRangedParam (param_ids::padOneShot (p),  0.0f);
                    setRangedParam (param_ids::padRelease (p),  0.8f);
                    setRangedParam (param_ids::padGlideMode (p),
                                    static_cast<float> (static_cast<int> (GlideMode::Legato)));
                    setRangedParam (param_ids::padGlideTime (p), 80.0f);
                    setRangedParam (param_ids::padMaxVoices (p), 1.0f);
                    setRangedParam (param_ids::padPitchEnvAmount (p), 3.0f);
                    setRangedParam (param_ids::padPitchEnvTime (p),  50.0f);
                    setRangedParam (param_ids::padChokeGroup (p), 0.0f);
                }
                break;

            default: break;
        }

        setStatus ("Loaded factory kit " + names[index]
                   + " - settings only, samples unchanged");
        return true;
    }

    // ---------------- Pad copy / paste ----------------

    bool ASamplerProcessor::copyPad (int padIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return false;

        copiedPad = juce::ValueTree ("copied_pad");
        const int p = padIndex + 1;

        // Store plain values keyed by spec index, so paste is a straight
        // replay and adding a parameter to padParamSpecs() covers both sides.
        for (std::size_t i = 0; i < padParamSpecs().size(); ++i)
        {
            if (auto* param = state.getParameter (padParamSpecs()[i].idFor (p)))
                copiedPad.setProperty ("p" + juce::String ((int) i),
                                       param->convertFrom0to1 (param->getValue()), nullptr);
        }

        for (int L = 0; L < kNumLayers; ++L)
            copiedPad.setProperty ("path" + juce::String (L),
                                   pads[padIndex].layers[(std::size_t) L].filePath, nullptr);

        setStatus ("Copied Pad " + juce::String (p));
        return true;
    }

    bool ASamplerProcessor::pastePad (int padIndex, bool includeSample)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return false;

        if (! copiedPad.isValid() || copiedPad.getNumProperties() == 0)
        {
            setStatus ("Nothing copied yet");
            return false;
        }

        const int p = padIndex + 1;
        beginUndoTransaction ("Paste to Pad " + juce::String (p));

        for (std::size_t i = 0; i < padParamSpecs().size(); ++i)
        {
            const auto key = "p" + juce::String ((int) i);
            if (! copiedPad.hasProperty (key))
                continue;

            setRangedParam (padParamSpecs()[i].idFor (p),
                            static_cast<float> (double (copiedPad.getProperty (key))));
        }

        if (includeSample)
        {
            for (int L = 0; L < kNumLayers; ++L)
            {
                const auto path = copiedPad.getProperty ("path" + juce::String (L), "").toString();
                auto& slot = pads[padIndex].layers[(std::size_t) L];

                if (path.isEmpty())
                {
                    slot.sample.store (defaultSample);
                    slot.filePath = {};
                    slot.missing  = false;
                    continue;
                }

                std::shared_ptr<const Sample> sample;
                if (readSampleFile (juce::File (path), sample) == LoadResult::Success)
                {
                    slot.sample.store (sample);
                    slot.filePath = path;
                    slot.missing  = false;
                }
                else
                {
                    slot.filePath = path;
                    slot.missing  = true;
                }
            }
        }

        setStatus ("Pasted onto Pad " + juce::String (p)
                   + (includeSample ? " with samples" : " (settings only)"));
        return true;
    }

    void ASamplerProcessor::clearAllPads()
    {
        beginUndoTransaction ("Clear all pads");

        for (int i = 0; i < kNumPads; ++i)
            for (int L = 0; L < kNumLayers; ++L)
            {
                auto& slot = pads[i].layers[(std::size_t) L];
                slot.sample.store (defaultSample);
                slot.filePath = {};
                slot.missing  = false;
                slot.processed.store (nullptr);
                slot.renderedTimeRatio = 1.0;
                slot.renderedFormant   = 0.0;
                slot.formantTable.store (nullptr);
                slot.tableKey = {};
            }

        setStatus ("Cleared all 16 pads - samples reset to default");
    }

    // ---------------- Offline stretch / formant rendering ----------------

    StretchOptions ASamplerProcessor::desiredStretchFor (int padIndex) const
    {
        StretchOptions options;

        if (padIndex < 0 || padIndex >= kNumPads)
            return options;

        const auto& p = padParams[(std::size_t) padIndex];
        if (p.stretchMode == nullptr || p.formant == nullptr)
            return options;

        // Formant shift applies whatever the tempo settings are — it is a
        // creative control, not a tempo one.
        options.formantSemitones = p.formant->load();

        // Time-stretch only when the pad is both tempo-synced and set to
        // Stretch. In Repitch mode the tempo match happens at playback rate
        // instead (see handleNoteOn), which is the classic sound and costs
        // nothing.
        const auto mode = static_cast<StretchMode> (
                              juce::roundToInt (p.stretchMode->load()));

        if (mode == StretchMode::Stretch
            && p.tempoSync != nullptr && p.tempoSync->load() >= 0.5f
            && p.originalBpm != nullptr)
        {
            const double originalBpm = p.originalBpm->load();
            const double hostBpm     = getHostBpm();

            if (originalBpm > 0.0 && hostBpm > 0.0)
            {
                // To play an `originalBpm` loop at `hostBpm`, its duration must
                // scale by originalBpm / hostBpm: a 90 BPM loop in an 80 BPM
                // project gets 12.5% longer.
                options.timeRatio = originalBpm / hostBpm;
            }
        }

        return options;
    }

    void ASamplerProcessor::requestRender (int padIndex, int layerIndex,
                                           const StretchOptions& options)
    {
        auto& slot = pads[padIndex].layers[(std::size_t) layerIndex];

        // Neutral: drop any previous render so the raw sample plays again.
        if (isNeutral (options))
        {
            if (slot.processed.load() != nullptr)
            {
                slot.processed.store (nullptr);
                ++slot.renderGeneration;
            }
            slot.renderedTimeRatio = 1.0;
            slot.renderedFormant   = 0.0;
            return;
        }

        auto source = slot.sample.load();
        if (source == nullptr || source->isEmpty())
            return;

        const int generation = ++slot.renderGeneration;
        slot.renderedTimeRatio = options.timeRatio;
        slot.renderedFormant   = options.formantSemitones;

        pendingRenders.fetch_add (1, std::memory_order_relaxed);

        loaderPool.addJob ([this, padIndex, layerIndex, generation, options, source]
        {
            const int numCh = source->getNumChannels();
            const int numS  = source->getNumSamples();

            juce::AudioBuffer<float> input (numCh, numS);
            for (int ch = 0; ch < numCh; ++ch)
                input.copyFrom (ch, 0, source->getReadPointer (ch), numS);

            auto rendered = processOffline (input, source->getSourceSampleRate(), options);

            auto result = std::make_shared<const Sample> (std::move (rendered),
                                                          source->getSourceSampleRate());

            juce::MessageManager::callAsync (
                [this, padIndex, layerIndex, generation, result]
                {
                    auto& s = pads[padIndex].layers[(std::size_t) layerIndex];

                    // A newer request superseded this one while it ran; its
                    // result is stale, so throw it away rather than clobbering.
                    if (s.renderGeneration == generation)
                        s.processed.store (result);

                    if (pendingRenders.fetch_sub (1, std::memory_order_acq_rel) == 1)
                        setStatus ("Stretch/formant render complete");
                });
        });
    }

    FormantTableKey ASamplerProcessor::desiredFormantTableFor (int padIndex,
                                                               int layerIndex) const
    {
        FormantTableKey key;

        if (padIndex < 0 || padIndex >= kNumPads) return key;
        if (layerIndex < 0 || layerIndex >= kNumLayers) return key;

        const auto& p = padParams[(std::size_t) padIndex];
        if (p.formantCorrect == nullptr || p.mode == nullptr)
            return key;

        if (p.formantCorrect->load() < 0.5f)
            return key;

        const auto stretch = desiredStretchFor (padIndex);

        // Span of note intervals this layer can be asked to play. A Fixed pad
        // always fires on one note, so that contributes nothing; in Key Zones
        // mode the layer has its own range and root, otherwise it inherits the
        // pad's.
        int noteLow  = 0;
        int noteHigh = 0;

        if (static_cast<PadMode> (juce::roundToInt (p.mode->load())) == PadMode::Keyboard)
        {
            const auto layerMode = static_cast<LayerMode> (
                                       juce::roundToInt (p.layerMode->load()));

            int lo   = juce::roundToInt (p.lowNote->load());
            int hi   = juce::roundToInt (p.highNote->load());
            int root = juce::roundToInt (p.note->load());

            if (layerMode == LayerMode::KeyZones)
            {
                const auto L = (std::size_t) layerIndex;
                lo   = juce::jmax (lo, juce::roundToInt (p.layerLowNote[L]->load()));
                hi   = juce::jmin (hi, juce::roundToInt (p.layerHighNote[L]->load()));
                root = juce::roundToInt (p.layerRootNote[L]->load());
            }

            if (hi < lo)
                return key;   // empty zone; nothing to render

            noteLow  = lo - root;
            noteHigh = hi - root;
        }

        // Everything else that shifts playback pitch, and therefore drags the
        // formants with it. The Pitch parameter is a constant offset; Vel->Pitch
        // spans zero to its full amount depending on how hard the pad is hit;
        // Repitch-mode tempo sync adds the tempo ratio in semitones.
        double constantOffset = (p.pitch != nullptr) ? p.pitch->load() : 0.0;

        if (p.tempoSync != nullptr && p.tempoSync->load() >= 0.5f
            && p.stretchMode != nullptr
            && static_cast<StretchMode> (juce::roundToInt (p.stretchMode->load()))
                   == StretchMode::Repitch
            && p.originalBpm != nullptr)
        {
            const double originalBpm = p.originalBpm->load();
            const double hostBpm     = getHostBpm();
            if (originalBpm > 0.0 && hostBpm > 0.0)
                constantOffset += 12.0 * std::log2 (hostBpm / originalBpm);
        }

        const double velAmount = (p.velToPitch != nullptr) ? p.velToPitch->load() : 0.0;
        const double velLow    = juce::jmin (0.0, velAmount);
        const double velHigh   = juce::jmax (0.0, velAmount);

        const int minOffset = static_cast<int> (
            std::floor (noteLow + constantOffset + velLow));
        const int maxOffset = static_cast<int> (
            std::ceil (noteHigh + constantOffset + velHigh));

        key.minOffset = juce::jlimit (-kMaxFormantOffset, kMaxFormantOffset, minOffset);
        key.maxOffset = juce::jlimit (-kMaxFormantOffset, kMaxFormantOffset, maxOffset);

        // A pad that only ever plays at its root pitch needs no table — the
        // single `processed` render already covers that one case.
        if (key.minOffset == 0 && key.maxOffset == 0)
            return key;

        key.enabled   = true;
        key.timeRatio = stretch.timeRatio;
        key.formant   = stretch.formantSemitones;

        return key;
    }

    std::shared_ptr<const FormantTable>
    ASamplerProcessor::buildFormantTable (const Sample& source,
                                          const FormantTableKey& key) const
    {
        auto table = std::make_shared<FormantTable>();

        const int numCh = source.getNumChannels();
        const int numS  = source.getNumSamples();
        const double sr = source.getSourceSampleRate();

        juce::AudioBuffer<float> input (numCh, numS);
        for (int ch = 0; ch < numCh; ++ch)
            input.copyFrom (ch, 0, source.getReadPointer (ch), numS);

        for (int offset = key.minOffset; offset <= key.maxOffset; ++offset)
        {
            StretchOptions options;
            options.timeRatio = key.timeRatio;

            // The whole trick: playing this variant at ratio 2^(offset/12) will
            // multiply its formants by that same ratio, so pre-shifting them
            // *down* by `offset` semitones leaves them where they started. The
            // user's own formant offset rides on top.
            options.formantSemitones = key.formant - offset;

            const int slot = offset + kMaxFormantOffset;
            if (slot < 0 || slot >= kFormantVariantSlots)
                continue;

            if (isNeutral (options))
            {
                // Offset 0 with no user shift needs no processing at all.
                table->variants[(std::size_t) slot] =
                    std::make_shared<const Sample> (juce::AudioBuffer<float> (input),
                                                    sr);
                continue;
            }

            auto rendered = processOffline (input, sr, options);
            table->variants[(std::size_t) slot] =
                std::make_shared<const Sample> (std::move (rendered), sr);
        }

        return table;
    }

    void ASamplerProcessor::requestFormantTable (int padIndex, int layerIndex,
                                                 const FormantTableKey& key)
    {
        auto& slot = pads[padIndex].layers[(std::size_t) layerIndex];

        if (! key.enabled)
        {
            if (slot.formantTable.load() != nullptr)
                slot.formantTable.store (nullptr);
            slot.tableKey = key;
            ++slot.tableGeneration;
            return;
        }

        auto source = slot.sample.load();
        if (source == nullptr || source->isEmpty())
            return;

        // Each variant is a full copy of the sample, so a long source across a
        // wide range is an unreasonable amount of memory. Refuse rather than
        // quietly allocating hundreds of megabytes, and say why.
        const int variants = key.maxOffset - key.minOffset + 1;
        const double seconds = source->getSourceSampleRate() > 0.0
                                 ? source->getNumSamples() / source->getSourceSampleRate()
                                 : 0.0;

        if (seconds * variants > 120.0)
        {
            setStatus ("Formant correction needs "
                       + juce::String (variants) + " renders of a "
                       + juce::String (seconds, 1) + "s sample - too much memory. "
                       "Narrow the pad's note range or use a shorter sample");
            slot.tableKey = key;   // don't retry every timer tick
            return;
        }

        slot.tableKey = key;
        const int generation = ++slot.tableGeneration;

        pendingRenders.fetch_add (1, std::memory_order_relaxed);
        setStatus ("Rendering " + juce::String (variants)
                   + " formant variants for Pad " + juce::String (padIndex + 1) + "...");

        loaderPool.addJob ([this, padIndex, layerIndex, generation, key, source]
        {
            auto table = buildFormantTable (*source, key);

            juce::MessageManager::callAsync (
                [this, padIndex, layerIndex, generation, table]
                {
                    auto& s = pads[padIndex].layers[(std::size_t) layerIndex];

                    if (s.tableGeneration == generation)
                        s.formantTable.store (table);

                    if (pendingRenders.fetch_sub (1, std::memory_order_acq_rel) == 1)
                        setStatus ("Formant correction ready");
                });
        });
    }

    void ASamplerProcessor::renderFormantTableNow (int padIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return;

        for (int L = 0; L < kNumLayers; ++L)
        {
            auto& slot = pads[padIndex].layers[(std::size_t) L];
            const auto key = desiredFormantTableFor (padIndex, L);

            if (! key.enabled)
            {
                slot.formantTable.store (nullptr);
                slot.tableKey = key;
                continue;
            }

            auto source = slot.sample.load();
            if (source == nullptr || source->isEmpty())
                continue;

            slot.formantTable.store (buildFormantTable (*source, key));
            slot.tableKey = key;
            ++slot.tableGeneration;
        }
    }

    int ASamplerProcessor::getFormantVariantCount (int padIndex, int layerIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return 0;
        if (layerIndex < 0 || layerIndex >= kNumLayers) return 0;

        auto table = pads[padIndex].layers[(std::size_t) layerIndex].formantTable.load();
        if (table == nullptr)
            return 0;

        int n = 0;
        for (const auto& v : table->variants)
            if (v != nullptr)
                ++n;
        return n;
    }

    void ASamplerProcessor::renderStretchNow (int padIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return;

        const auto wanted = desiredStretchFor (padIndex);

        for (int L = 0; L < kNumLayers; ++L)
        {
            auto& slot = pads[padIndex].layers[(std::size_t) L];

            auto source = slot.sample.load();
            if (source == nullptr || source->isEmpty())
                continue;

            if (isNeutral (wanted))
            {
                slot.processed.store (nullptr);
                slot.renderedTimeRatio = 1.0;
                slot.renderedFormant   = 0.0;
                continue;
            }

            const int numCh = source->getNumChannels();
            const int numS  = source->getNumSamples();

            juce::AudioBuffer<float> input (numCh, numS);
            for (int ch = 0; ch < numCh; ++ch)
                input.copyFrom (ch, 0, source->getReadPointer (ch), numS);

            auto rendered = processOffline (input, source->getSourceSampleRate(), wanted);

            slot.processed.store (std::make_shared<const Sample> (
                std::move (rendered), source->getSourceSampleRate()));

            // Record the key so the timer doesn't immediately redo this work.
            slot.renderedTimeRatio = wanted.timeRatio;
            slot.renderedFormant   = wanted.formantSemitones;
            ++slot.renderGeneration;
        }
    }

    bool ASamplerProcessor::isPadRendered (int padIndex, int layerIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return false;
        if (layerIndex < 0 || layerIndex >= kNumLayers) return false;
        return pads[padIndex].layers[(std::size_t) layerIndex].processed.load() != nullptr;
    }

    void ASamplerProcessor::timerCallback()
    {
        // Poll rather than listen, because the target depends on host tempo as
        // well as on parameters, and tempo changes are not parameter changes.
        for (int p = 0; p < kNumPads; ++p)
        {
            const auto wanted = desiredStretchFor (p);

            for (int L = 0; L < kNumLayers; ++L)
            {
                auto& slot = pads[p].layers[(std::size_t) L];

                if (slot.sample.load() == nullptr)
                    continue;

                // requestRender records what it was asked for, so "already
                // asked for exactly this" is the whole up-to-date test —
                // including a render still in flight, which must not be
                // requested twice.
                const bool alreadyRequested =
                    std::abs (slot.renderedTimeRatio - wanted.timeRatio)      < 1.0e-6
                 && std::abs (slot.renderedFormant  - wanted.formantSemitones) < 1.0e-6;

                if (! alreadyRequested)
                    requestRender (p, L, wanted);

                // Per-note formant correction table. Debounced: a table render
                // is expensive and the Pitch knob walks the required range a
                // semitone at a time, so wait for the target to hold still
                // before committing to the work.
                const auto wantedTable = desiredFormantTableFor (p, L);

                if (slot.tableKey == wantedTable)
                {
                    slot.pendingTableTicks = 0;
                }
                else if (slot.pendingTableKey == wantedTable)
                {
                    if (++slot.pendingTableTicks >= 2)
                    {
                        slot.pendingTableTicks = 0;
                        requestFormantTable (p, L, wantedTable);
                    }
                }
                else
                {
                    slot.pendingTableKey   = wantedTable;
                    slot.pendingTableTicks = 0;
                }
            }
        }
    }

    // ---------------- Snap slice window to transients ----------------

    bool ASamplerProcessor::snapPadWindowToTransients (int padIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return false;

        auto sample = pads[padIndex].layers[(std::size_t) getCurrentEditLayer()].sample.load();
        if (sample == nullptr || sample->isEmpty() || sample->getNumSamples() < 2)
        {
            setStatus ("Pad " + juce::String (padIndex + 1) + " has no sample to analyse");
            return false;
        }

        // Copy into a buffer the detector can read (it takes a non-const
        // AudioBuffer). One-shot user action, so the alloc is irrelevant.
        const int numCh = sample->getNumChannels();
        const int numS  = sample->getNumSamples();
        juce::AudioBuffer<float> audio (numCh, numS);
        for (int ch = 0; ch < numCh; ++ch)
            audio.copyFrom (ch, 0, sample->getReadPointer (ch), numS);

        // Spectral flux, because this is aimed at melodic material (a guitar
        // riff, a chopped loop) where log-energy misses soft attacks.
        const auto hits = detectTransients (audio, sample->getSourceSampleRate(),
                                            OnsetMethod::SpectralFlux);
        if (hits.empty())
        {
            setStatus ("No transients found on Pad " + juce::String (padIndex + 1));
            return false;
        }

        const auto lastIndex = static_cast<float> (numS - 1);
        const float startNorm = padParams[(std::size_t) padIndex].start->load();
        const float endNorm   = padParams[(std::size_t) padIndex].end->load();

        // Nearest onset to each boundary, in sample terms.
        auto nearestOnset = [&hits] (int target)
        {
            int best = hits[0].sampleIndex;
            int bestDist = std::abs (target - best);
            for (const auto& h : hits)
            {
                const int d = std::abs (target - h.sampleIndex);
                if (d < bestDist) { bestDist = d; best = h.sampleIndex; }
            }
            return best;
        };

        const int startTarget = juce::roundToInt (startNorm * lastIndex);
        const int endTarget   = juce::roundToInt (endNorm   * lastIndex);

        int newStart = nearestOnset (startTarget);
        int newEnd   = nearestOnset (endTarget);

        // The end boundary usually wants the onset of the *next* slice, not the
        // one it sits on — snapping both to the same hit would collapse the
        // window. If they collide, push the end to the next onset after start.
        if (newEnd <= newStart)
        {
            int candidate = -1;
            for (const auto& h : hits)
                if (h.sampleIndex > newStart) { candidate = h.sampleIndex; break; }

            newEnd = (candidate > newStart) ? candidate : (numS - 1);
        }

        beginUndoTransaction ("Snap Pad " + juce::String (padIndex + 1) + " to transients");

        setRangedParam (param_ids::padStart (padIndex + 1),
                        juce::jlimit (0.0f, 1.0f, static_cast<float> (newStart) / lastIndex));
        setRangedParam (param_ids::padEnd (padIndex + 1),
                        juce::jlimit (0.0f, 1.0f, static_cast<float> (newEnd) / lastIndex));

        setStatus ("Snapped Pad " + juce::String (padIndex + 1)
                   + " window to nearest transients ("
                   + juce::String (hits.size()) + " found)");
        return true;
    }

    // ---------------- Slice -> MIDI ----------------

    bool ASamplerProcessor::exportSliceMidi (const juce::File& file, int numSlices)
    {
        numSlices = juce::jlimit (1, kNumPads, numSlices);

        // One note per slice on a 16th-note grid. Ticks-per-quarter-note of 960
        // is the usual DAW-friendly resolution, so a 16th is 240 ticks.
        constexpr int ticksPerQuarter = 960;
        constexpr int ticksPerStep    = ticksPerQuarter / 4;

        juce::MidiMessageSequence seq;
        for (int i = 0; i < numSlices; ++i)
        {
            const int note = juce::jlimit (0, 127,
                juce::roundToInt (padParams[(std::size_t) i].note->load()));

            const double on  = static_cast<double> (i * ticksPerStep);
            const double off = on + ticksPerStep * 0.9;   // slightly detached

            seq.addEvent (juce::MidiMessage::noteOn  (1, note, 0.8f), on);
            seq.addEvent (juce::MidiMessage::noteOff (1, note),       off);
        }
        seq.updateMatchedPairs();

        juce::MidiFile midiFile;
        midiFile.setTicksPerQuarterNote (ticksPerQuarter);

        // Track 0 carries the tempo so the pattern imports at the project's
        // tempo rather than a default 120.
        juce::MidiMessageSequence tempoTrack;
        tempoTrack.addEvent (juce::MidiMessage::tempoMetaEvent (
            juce::roundToInt (60000000.0 / getHostBpm())), 0.0);
        midiFile.addTrack (tempoTrack);
        midiFile.addTrack (seq);

        file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());
        if (out == nullptr || ! out->openedOk())
        {
            setStatus ("Could not write MIDI to " + file.getFileName());
            return false;
        }

        midiFile.writeTo (*out);
        out->flush();

        setStatus ("Exported " + juce::String (numSlices) + " slices to "
                   + file.getFileName() + " in " + file.getParentDirectory().getFileName());
        return true;
    }

    // ---------------- Audition ----------------

    void ASamplerProcessor::auditionPad (int padIndex, float velocity)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return;

        // Hand off to the audio thread rather than triggering here — voices are
        // owned by processBlock and must not be started from the message thread.
        const int encoded = juce::jlimit (1, 1000,
            juce::roundToInt (juce::jlimit (0.0f, 1.0f, velocity) * 1000.0f));
        pendingAudition[(std::size_t) padIndex].store (encoded, std::memory_order_release);

        setStatus ("Audition Pad " + juce::String (padIndex + 1)
                   + ": " + getPadSampleDisplayName (padIndex));
    }

    // ---------------- State persistence ----------------

    void ASamplerProcessor::getStateInformation (juce::MemoryBlock& dest)
    {
        auto root = state.copyState();

        // Bundle pad-sample paths alongside the parameter tree so a project
        // reload restores both. Each entry carries both pad index and layer
        // index — older states without the "layer" attribute default to 0
        // when reloaded, preserving backward compatibility.
        auto samples = juce::ValueTree ("samples");
        for (int i = 0; i < kNumPads; ++i)
        {
            for (int L = 0; L < kNumLayers; ++L)
            {
                const auto& path = pads[i].layers[(std::size_t) L].filePath;
                if (path.isEmpty()) continue;

                auto entry = juce::ValueTree ("pad");
                entry.setProperty ("index", i, nullptr);
                entry.setProperty ("layer", L, nullptr);
                entry.setProperty ("path",  path, nullptr);
                samples.appendChild (entry, nullptr);
            }
        }
        root.addChild (samples, -1, nullptr);

        // CC -> parameter-ID bindings. We serialise the *ID* (not the index)
        // so a future reorder of getParameters() doesn't silently remap
        // every learned controller.
        auto learn = juce::ValueTree ("midilearn");
        const auto& params = getParameters();
        for (int cc = 0; cc < 128; ++cc)
        {
            const int idx = ccBindings[(std::size_t) cc].load (std::memory_order_relaxed);
            if (idx < 0 || idx >= params.size()) continue;
            if (auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (params[idx]))
            {
                auto entry = juce::ValueTree ("cc");
                entry.setProperty ("number",  cc, nullptr);
                entry.setProperty ("paramId", withId->paramID, nullptr);
                learn.appendChild (entry, nullptr);
            }
        }
        root.addChild (learn, -1, nullptr);

        if (auto xml = root.createXml())
            copyXmlToBinary (*xml, dest);
    }

    void ASamplerProcessor::setStateInformation (const void* data, int size)
    {
        auto xml = getXmlFromBinary (data, size);
        if (xml == nullptr) return;

        auto root = juce::ValueTree::fromXml (*xml);
        if (! root.isValid()) return;

        // Pull samples and midilearn out of the tree before handing the
        // rest to the param state (replaceState would otherwise expose
        // these nodes to the parameter system).
        auto samples = root.getChildWithName ("samples");
        if (samples.isValid())
            root.removeChild (samples, nullptr);

        auto learn = root.getChildWithName ("midilearn");
        if (learn.isValid())
            root.removeChild (learn, nullptr);

        state.replaceState (root);

        for (auto& pad : pads)
        {
            for (auto& layer : pad.layers)
            {
                layer.sample.store (defaultSample);
                layer.filePath = {};
                layer.missing  = false;
                layer.processed.store (nullptr);
                layer.renderedTimeRatio = 1.0;
                layer.renderedFormant   = 0.0;
                layer.formantTable.store (nullptr);
                layer.tableKey = {};
            }
        }

        if (samples.isValid())
        {
            int restored = 0;
            int missing  = 0;

            for (int i = 0; i < samples.getNumChildren(); ++i)
            {
                auto entry = samples.getChild (i);
                const int  idx   = entry.getProperty ("index", -1);
                const int  layer = entry.getProperty ("layer", 0);  // legacy: layer 0
                const auto path  = entry.getProperty ("path", "").toString();

                if (idx < 0 || idx >= kNumPads
                    || layer < 0 || layer >= kNumLayers
                    || path.isEmpty())
                    continue;

                std::shared_ptr<const Sample> sample;
                auto& slot = pads[idx].layers[(std::size_t) layer];

                if (readSampleFile (juce::File (path), sample) == LoadResult::Success)
                {
                    slot.sample.store (sample);
                    slot.filePath = path;
                    slot.missing  = false;
                    ++restored;
                }
                else
                {
                    // Keep the reference so it survives a re-save and can be
                    // repaired by relinkMissingSamples() — dropping it here
                    // would quietly lose the user's kit.
                    slot.filePath = path;
                    slot.missing  = true;
                    ++missing;
                }
            }

            // One summary rather than a burst of per-sample announcements, and
            // only when there is something the user needs to act on.
            if (missing > 0)
                setStatus ("Restored " + juce::String (restored) + " samples, "
                           + juce::String (missing) + " missing"
                           + " - use Relink to locate them");
        }

        // Clear all bindings first so a partial save (or a legacy save with
        // no <midilearn> node) ends up in a clean state, not a mix.
        for (auto& binding : ccBindings)
            binding.store (-1, std::memory_order_relaxed);

        if (learn.isValid())
        {
            for (int i = 0; i < learn.getNumChildren(); ++i)
            {
                auto entry      = learn.getChild (i);
                const int  cc   = entry.getProperty ("number", -1);
                const auto pid  = entry.getProperty ("paramId", "").toString();
                if (cc < 0 || cc > 127 || pid.isEmpty()) continue;
                bindCcToParameter (cc, pid);
            }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new asampler::ASamplerProcessor();
}
