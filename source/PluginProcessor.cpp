#include "PluginProcessor.h"
#include "engine/SampleAnalysis.h"
#include "PluginEditor.h"

#include "engine/DefaultSample.h"
#include "engine/PhaseVocoder.h"
#include "engine/Transient.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <thread>
#include <vector>

namespace asampler
{
    namespace
    {
        // juce::AudioParameterBool stores whatever normalised value a host
        // writes (0.235, say) while APVTS reads it back snapped to 0 or 1. A
        // state restore then sees "already 0" and skips the write, leaving the
        // raw 0.235 behind, so the host's parameter list disagrees with the
        // restored project. Report the snapped value instead.
        class SnappedBoolParameter final : public juce::AudioParameterBool
        {
        public:
            using juce::AudioParameterBool::AudioParameterBool;
            float getValue() const override { return get() ? 1.0f : 0.0f; }
        };

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

        // Slice points are stored as a 0..1 fraction of the sample, but read and
        // typed as time: "25%" says nothing about where a hit lands, "1.250 s"
        // does. `lengthSeconds` <= 0 (length unknown) falls back to percent.
        juce::String formatSlicePoint (float norm, double lengthSeconds)
        {
            if (! (lengthSeconds > 0.0))
                return juce::String (norm * 100.0f, 2) + "%";

            const double t = static_cast<double> (norm) * lengthSeconds;
            return t < 1.0 ? juce::String (t * 1000.0, 1) + " ms"
                           : juce::String (t, 3) + " s";
        }

        // Inverse of formatSlicePoint, for values typed into the host's
        // parameter field. Accepts "1.5 s", "1500 ms", "25%", or a bare number,
        // which is read as seconds (the unit the display uses above 1 s). The
        // result is clamped to the sample.
        float parseSlicePoint (const juce::String& text, double lengthSeconds)
        {
            auto t = text.trim().toLowerCase().removeCharacters (" ");
            const double number = t.getDoubleValue();

            double norm = 0.0;
            if (t.endsWithChar ('%') || ! (lengthSeconds > 0.0))
                norm = number / 100.0;
            else if (t.endsWith ("ms"))
                norm = (number / 1000.0) / lengthSeconds;
            else
                norm = number / lengthSeconds;   // "s" suffix or bare seconds

            return static_cast<float> (juce::jlimit (0.0, 1.0, norm));
        }

        // A duration in the same units the slice points use.
        juce::String formatDuration (double seconds)
        {
            return seconds < 1.0 ? juce::String (seconds * 1000.0, 1) + " ms"
                                 : juce::String (seconds, 3) + " s";
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

        // Chop count choices — must mirror chopCountFromIndex() below. 1:1
        // with the AudioParameterChoice index stored in saved state, so new
        // counts are appended; the default stays 16 (one bank).
        const juce::StringArray kChopCountLabels { "2", "4", "8", "16", "32" };
        constexpr int kDefaultChopIndex = 3;

        constexpr int chopCountFromIndex (int choiceIndex) noexcept
        {
            switch (choiceIndex)
            {
                case 0: return 2;
                case 1: return 4;
                case 2: return 8;
                case 3: return 16;
                case 4: return 32;
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
        const juce::StringArray kEditLayerLabels { "Layer 1", "Layer 2", "Layer 3", "Layer 4",
                                                   "Layer 5", "Layer 6", "Layer 7", "Layer 8" };
        static_assert (kNumLayers == 8,
                       "Edit-layer labels must match kNumLayers");

        // Per-layer trigger edge. Append-only (serialised by index).
        const juce::StringArray kLayerTriggerLabels { "Press", "Release" };

        // Layers 1-4 keep the four equal velocity bands they always had, so a
        // kit made before the ranges existed plays the same; layers 5-8 cover
        // every velocity until the user narrows them.
        constexpr int defaultLayerVelLow  (int layerOneBased) noexcept { return layerOneBased <= 4 ? (layerOneBased - 1) * 32 : 0; }
        constexpr int defaultLayerVelHigh (int layerOneBased) noexcept { return layerOneBased <= 4 ? layerOneBased * 32 - 1 : 127; }

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

        // Keep aligned with enum class LfoSync / ModDest in engine/Lfo.h.
        const juce::StringArray kLfoSyncLabels { "Off", "4 bars", "2 bars", "1 bar",
                                                 "1/2", "1/4", "1/8", "1/16", "1/32",
                                                 "1/2 triplet", "1/4 triplet", "1/8 triplet", "1/16 triplet",
                                                 "1/2 dotted", "1/4 dotted", "1/8 dotted" };
        const juce::StringArray kModDestLabels { "Off", "Pitch", "Filter Cutoff", "Volume", "Pan",
                                                 "LFO 1 Depth", "LFO 2 Depth" };

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
            const char*   section;         // sub-group: "Filter", "Slice", ...
        };

        const std::vector<PadParamSpec>& padParamSpecs()
        {
            static const std::vector<PadParamSpec> specs = {
                { [] (int p) { return param_ids::padVolume (p); }, "Volume", "Main" },
                { [] (int p) { return param_ids::padPan (p); }, "Pan", "Main" },
                { [] (int p) { return param_ids::padPitch (p); }, "Pitch", "Main" },
                { [] (int p) { return param_ids::padNote (p); }, "Note", "Trigger" },
                { [] (int p) { return param_ids::padMode (p); }, "Mode", "Trigger" },
                { [] (int p) { return param_ids::padLowNote (p); }, "Low Note", "Trigger" },
                { [] (int p) { return param_ids::padHighNote (p); }, "High Note", "Trigger" },
                { [] (int p) { return param_ids::padAttack (p); }, "Attack", "Envelope" },
                { [] (int p) { return param_ids::padDecay (p); }, "Decay", "Envelope" },
                { [] (int p) { return param_ids::padSustain (p); }, "Sustain", "Envelope" },
                { [] (int p) { return param_ids::padRelease (p); }, "Release", "Envelope" },
                { [] (int p) { return param_ids::padChokeGroup (p); }, "Choke Group", "Main" },
                { [] (int p) { return param_ids::padStart (p); }, "Start", "Slice" },
                { [] (int p) { return param_ids::padEnd (p); }, "End", "Slice" },
                { [] (int p) { return param_ids::padReverse (p); }, "Reverse", "Main" },
                { [] (int p) { return param_ids::padVelocityCurve (p); }, "Velocity Curve", "Trigger" },
                { [] (int p) { return param_ids::padFilterMode (p); }, "Filter Mode", "Filter" },
                { [] (int p) { return param_ids::padFilterCutoff (p); }, "Filter Cutoff", "Filter" },
                { [] (int p) { return param_ids::padFilterResonance (p); }, "Filter Resonance", "Filter" },
                { [] (int p) { return param_ids::padLoopMode (p); }, "Loop Mode", "Slice" },
                { [] (int p) { return param_ids::padLoopStart (p); }, "Loop Start", "Slice" },
                { [] (int p) { return param_ids::padLoopEnd (p); }, "Loop End", "Slice" },
                { [] (int p) { return param_ids::padLayerMode (p); }, "Layer Mode", "Layers" },
                { [] (int p) { return param_ids::padRepeatEnabled (p); }, "Repeat", "Repeat" },
                { [] (int p) { return param_ids::padRepeatRate (p); }, "Repeat Rate", "Repeat" },
                { [] (int p) { return param_ids::padOneShot (p); }, "One Shot", "Main" },
                { [] (int p) { return param_ids::padMute (p); }, "Mute", "Main" },
                { [] (int p) { return param_ids::padSolo (p); }, "Solo", "Main" },
                { [] (int p) { return param_ids::padGlideMode (p); }, "Glide Mode", "Pitch and Glide" },
                { [] (int p) { return param_ids::padGlideTime (p); }, "Glide Time", "Pitch and Glide" },
                { [] (int p) { return param_ids::padMaxVoices (p); }, "Max Voices", "Main" },
                { [] (int p) { return param_ids::padSatMode (p); }, "Saturation Mode", "Character" },
                { [] (int p) { return param_ids::padSatAmount (p); }, "Saturation", "Character" },
                { [] (int p) { return param_ids::padBitDepth (p); }, "Bit Depth", "Character" },
                { [] (int p) { return param_ids::padDownsample (p); }, "SR Reduce", "Character" },
                { [] (int p) { return param_ids::padPitchEnvAmount (p); }, "Pitch Env Amount", "Pitch and Glide" },
                { [] (int p) { return param_ids::padPitchEnvTime (p); }, "Pitch Env Time", "Pitch and Glide" },
                { [] (int p) { return param_ids::padFadeIn (p); }, "Fade In", "Slice" },
                { [] (int p) { return param_ids::padFadeOut (p); }, "Fade Out", "Slice" },
                { [] (int p) { return param_ids::padLoopXfade (p); }, "Loop Crossfade", "Slice" },
                { [] (int p) { return param_ids::padZeroSnap (p); }, "Zero-Cross Snap", "Slice" },
                { [] (int p) { return param_ids::padLfoRate (p); }, "LFO Rate", "Modulation" },
                { [] (int p) { return param_ids::padLfoDepth (p); }, "LFO Depth", "Modulation" },
                { [] (int p) { return param_ids::padLfoShape (p); }, "LFO Shape", "Modulation" },
                { [] (int p) { return param_ids::padLfoDest (p); }, "LFO Destination", "Modulation" },
                { [] (int p) { return param_ids::padVelToCutoff (p); }, "Vel to Cutoff", "Modulation" },
                { [] (int p) { return param_ids::padVelToPitch (p); }, "Vel to Pitch", "Modulation" },
                { [] (int p) { return param_ids::padVelToStart (p); }, "Vel to Start", "Modulation" },
                { [] (int p) { return param_ids::padSwing (p); }, "Swing", "Repeat" },
                { [] (int p) { return param_ids::padHumanize (p); }, "Humanize", "Repeat" },
                { [] (int p) { return param_ids::padTempoSync (p); }, "Tempo Sync", "Time and Formant" },
                { [] (int p) { return param_ids::padOriginalBpm (p); }, "Original BPM", "Time and Formant" },
                { [] (int p) { return param_ids::padVelToGlide (p); }, "Vel to Glide", "Pitch and Glide" },
                { [] (int p) { return param_ids::padFiltEnvAmount (p); }, "Filter Env Amount", "Filter" },
                { [] (int p) { return param_ids::padFiltEnvAttack (p); }, "Filter Env Attack", "Filter" },
                { [] (int p) { return param_ids::padFiltEnvDecay (p); }, "Filter Env Decay", "Filter" },
                { [] (int p) { return param_ids::padFiltEnvSustain (p); }, "Filter Env Sustain", "Filter" },
                { [] (int p) { return param_ids::padFiltEnvRelease (p); }, "Filter Env Release", "Filter" },
                { [] (int p) { return param_ids::padStereoWidth (p); }, "Stereo Width", "Stereo" },
                { [] (int p) { return param_ids::padMonoBelow (p); }, "Mono Below", "Stereo" },
                { [] (int p) { return param_ids::padScaleLock (p); }, "Scale Lock", "Trigger" },
                { [] (int p) { return param_ids::padStretchMode (p); }, "Stretch Mode", "Time and Formant" },
                { [] (int p) { return param_ids::padFormant (p); }, "Formant", "Time and Formant" },
                { [] (int p) { return param_ids::padFormantCorrect (p); }, "Formant Correct", "Time and Formant" },
                { [] (int p) { return param_ids::padLayerGain (p, 1); }, "Layer 1 Gain", "Layers" },
                { [] (int p) { return param_ids::padLayerGain (p, 2); }, "Layer 2 Gain", "Layers" },
                { [] (int p) { return param_ids::padLayerGain (p, 3); }, "Layer 3 Gain", "Layers" },
                { [] (int p) { return param_ids::padLayerGain (p, 4); }, "Layer 4 Gain", "Layers" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 1); }, "Layer 1 Low Note", "Layers" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 1); }, "Layer 1 High Note", "Layers" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 1); }, "Layer 1 Root Note", "Layers" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 2); }, "Layer 2 Low Note", "Layers" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 2); }, "Layer 2 High Note", "Layers" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 2); }, "Layer 2 Root Note", "Layers" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 3); }, "Layer 3 Low Note", "Layers" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 3); }, "Layer 3 High Note", "Layers" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 3); }, "Layer 3 Root Note", "Layers" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 4); }, "Layer 4 Low Note", "Layers" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 4); }, "Layer 4 High Note", "Layers" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 4); }, "Layer 4 Root Note", "Layers" },
                { [] (int p) { return param_ids::padLfoSync (p); }, "LFO Sync", "Modulation" },
                { [] (int p) { return param_ids::padLfo2Rate (p); }, "LFO 2 Rate", "Modulation" },
                { [] (int p) { return param_ids::padLfo2Depth (p); }, "LFO 2 Depth", "Modulation" },
                { [] (int p) { return param_ids::padLfo2Shape (p); }, "LFO 2 Shape", "Modulation" },
                { [] (int p) { return param_ids::padLfo2Dest (p); }, "LFO 2 Destination", "Modulation" },
                { [] (int p) { return param_ids::padLfo2Sync (p); }, "LFO 2 Sync", "Modulation" },
                { [] (int p) { return param_ids::padModWheelDest (p); }, "Mod Wheel Destination", "Modulation" },
                { [] (int p) { return param_ids::padModWheelAmount (p); }, "Mod Wheel Amount", "Modulation" },
                { [] (int p) { return param_ids::padLayerGain (p, 5); }, "Layer 5 Gain", "Layers" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 5); }, "Layer 5 Low Note", "Layers" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 5); }, "Layer 5 High Note", "Layers" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 5); }, "Layer 5 Root Note", "Layers" },
                { [] (int p) { return param_ids::padLayerGain (p, 6); }, "Layer 6 Gain", "Layers" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 6); }, "Layer 6 Low Note", "Layers" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 6); }, "Layer 6 High Note", "Layers" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 6); }, "Layer 6 Root Note", "Layers" },
                { [] (int p) { return param_ids::padLayerGain (p, 7); }, "Layer 7 Gain", "Layers" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 7); }, "Layer 7 Low Note", "Layers" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 7); }, "Layer 7 High Note", "Layers" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 7); }, "Layer 7 Root Note", "Layers" },
                { [] (int p) { return param_ids::padLayerGain (p, 8); }, "Layer 8 Gain", "Layers" },
                { [] (int p) { return param_ids::padLayerLowNote (p, 8); }, "Layer 8 Low Note", "Layers" },
                { [] (int p) { return param_ids::padLayerHighNote (p, 8); }, "Layer 8 High Note", "Layers" },
                { [] (int p) { return param_ids::padLayerRootNote (p, 8); }, "Layer 8 Root Note", "Layers" },
                { [] (int p) { return param_ids::padLayerVelLow (p, 1); }, "Layer 1 Vel Low", "Layers" },
                { [] (int p) { return param_ids::padLayerVelHigh (p, 1); }, "Layer 1 Vel High", "Layers" },
                { [] (int p) { return param_ids::padLayerTrigger (p, 1); }, "Layer 1 Trigger", "Layers" },
                { [] (int p) { return param_ids::padLayerTranspose (p, 1); }, "Layer 1 Transpose", "Layers" },
                { [] (int p) { return param_ids::padLayerFine (p, 1); }, "Layer 1 Fine Tune", "Layers" },
                { [] (int p) { return param_ids::padLayerVelLow (p, 2); }, "Layer 2 Vel Low", "Layers" },
                { [] (int p) { return param_ids::padLayerVelHigh (p, 2); }, "Layer 2 Vel High", "Layers" },
                { [] (int p) { return param_ids::padLayerTrigger (p, 2); }, "Layer 2 Trigger", "Layers" },
                { [] (int p) { return param_ids::padLayerTranspose (p, 2); }, "Layer 2 Transpose", "Layers" },
                { [] (int p) { return param_ids::padLayerFine (p, 2); }, "Layer 2 Fine Tune", "Layers" },
                { [] (int p) { return param_ids::padLayerVelLow (p, 3); }, "Layer 3 Vel Low", "Layers" },
                { [] (int p) { return param_ids::padLayerVelHigh (p, 3); }, "Layer 3 Vel High", "Layers" },
                { [] (int p) { return param_ids::padLayerTrigger (p, 3); }, "Layer 3 Trigger", "Layers" },
                { [] (int p) { return param_ids::padLayerTranspose (p, 3); }, "Layer 3 Transpose", "Layers" },
                { [] (int p) { return param_ids::padLayerFine (p, 3); }, "Layer 3 Fine Tune", "Layers" },
                { [] (int p) { return param_ids::padLayerVelLow (p, 4); }, "Layer 4 Vel Low", "Layers" },
                { [] (int p) { return param_ids::padLayerVelHigh (p, 4); }, "Layer 4 Vel High", "Layers" },
                { [] (int p) { return param_ids::padLayerTrigger (p, 4); }, "Layer 4 Trigger", "Layers" },
                { [] (int p) { return param_ids::padLayerTranspose (p, 4); }, "Layer 4 Transpose", "Layers" },
                { [] (int p) { return param_ids::padLayerFine (p, 4); }, "Layer 4 Fine Tune", "Layers" },
                { [] (int p) { return param_ids::padLayerVelLow (p, 5); }, "Layer 5 Vel Low", "Layers" },
                { [] (int p) { return param_ids::padLayerVelHigh (p, 5); }, "Layer 5 Vel High", "Layers" },
                { [] (int p) { return param_ids::padLayerTrigger (p, 5); }, "Layer 5 Trigger", "Layers" },
                { [] (int p) { return param_ids::padLayerTranspose (p, 5); }, "Layer 5 Transpose", "Layers" },
                { [] (int p) { return param_ids::padLayerFine (p, 5); }, "Layer 5 Fine Tune", "Layers" },
                { [] (int p) { return param_ids::padLayerVelLow (p, 6); }, "Layer 6 Vel Low", "Layers" },
                { [] (int p) { return param_ids::padLayerVelHigh (p, 6); }, "Layer 6 Vel High", "Layers" },
                { [] (int p) { return param_ids::padLayerTrigger (p, 6); }, "Layer 6 Trigger", "Layers" },
                { [] (int p) { return param_ids::padLayerTranspose (p, 6); }, "Layer 6 Transpose", "Layers" },
                { [] (int p) { return param_ids::padLayerFine (p, 6); }, "Layer 6 Fine Tune", "Layers" },
                { [] (int p) { return param_ids::padLayerVelLow (p, 7); }, "Layer 7 Vel Low", "Layers" },
                { [] (int p) { return param_ids::padLayerVelHigh (p, 7); }, "Layer 7 Vel High", "Layers" },
                { [] (int p) { return param_ids::padLayerTrigger (p, 7); }, "Layer 7 Trigger", "Layers" },
                { [] (int p) { return param_ids::padLayerTranspose (p, 7); }, "Layer 7 Transpose", "Layers" },
                { [] (int p) { return param_ids::padLayerFine (p, 7); }, "Layer 7 Fine Tune", "Layers" },
                { [] (int p) { return param_ids::padLayerVelLow (p, 8); }, "Layer 8 Vel Low", "Layers" },
                { [] (int p) { return param_ids::padLayerVelHigh (p, 8); }, "Layer 8 Vel High", "Layers" },
                { [] (int p) { return param_ids::padLayerTrigger (p, 8); }, "Layer 8 Trigger", "Layers" },
                { [] (int p) { return param_ids::padLayerTranspose (p, 8); }, "Layer 8 Transpose", "Layers" },
                { [] (int p) { return param_ids::padLayerFine (p, 8); }, "Layer 8 Fine Tune", "Layers" },
                { [] (int p) { return param_ids::padSpeed (p); }, "Speed", "Time and Formant" },
                { [] (int p) { return param_ids::padKeepLength (p); }, "Keep Length", "Time and Formant" },
            };
            return specs;
        }

        // Stable host ID for a Focus proxy: "pad_1_cutoff" -> "focus_cutoff".
        // A real ID (rather than JUCE's index fallback) keeps saved automation
        // on Focus parameters pointing at the same control when parameters
        // are added or regrouped.
        juce::String focusProxyIdFor (const PadParamSpec& spec)
        {
            return "focus_" + spec.idFor (1).fromFirstOccurrenceOf ("pad_1_", false, false);
        }

        std::unique_ptr<juce::AudioProcessorParameterGroup>
        makePadGroup (int pad, const ASamplerProcessor::PadLengthFn& padSeconds)
        {
            using namespace juce;

            // Slice-point attributes: shown and typed as time in the pad's
            // current sample. The step is fine enough for ~0.2 ms at 3 minutes.
            const auto slicePointAttributes = [pad, padSeconds]
            {
                return AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([pad, padSeconds] (float v, int)
                        { return formatSlicePoint (v, padSeconds ? padSeconds (pad - 1) : 0.0); })
                    .withValueFromStringFunction ([pad, padSeconds] (const String& text)
                        { return parseSlicePoint (text, padSeconds ? padSeconds (pad - 1) : 0.0); });
            };

            // Parameters are made first and filed into section sub-groups
            // afterwards, in padParamSpecs() order (see the end of this
            // function), so the sectioning lives in one table.
            std::map<juce::String, std::unique_ptr<RangedAudioParameter>> made;
            auto add = [&made] (std::unique_ptr<RangedAudioParameter> p)
            {
                auto id = p->paramID;
                made[id] = std::move (p);
            };

            // "P9 Filter Cutoff": the pad number travels with the name, because
            // automation lanes, MIDI-learn readouts and most hosts' parameter
            // lists show the name alone, without the "Pad 9" group around it.
            const auto nm = [pad] (const juce::String& name) { return "P" + juce::String (pad) + " " + name; };

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVolume (pad), 1 }, nm ("Volume"),
                NormalisableRange<float> { -60.0f, 12.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("dB")
                    .withStringFromValueFunction ([] (float v, int) { return formatDb (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padPan (pad), 1 }, nm ("Pan"),
                NormalisableRange<float> { -1.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPan (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padPitch (pad), 1 }, nm ("Pitch"),
                NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("st")
                    .withStringFromValueFunction ([] (float v, int) { return formatSemitones (v); })));

            add (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padNote (pad), 1 }, nm ("Note"),
                0, 127, defaultNoteForPad (pad),
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

            // Note-dispatch mode. Default Fixed preserves every existing kit:
            // the pad fires only on `Note`. Keyboard mode re-purposes `Note`
            // as the root and listens across [Low Note, High Note] — incoming
            // notes pitch the sample relative to root, so one pad can cover
            // an 808's full range while neighbouring pads stay on fixed drum
            // notes in the same kit.
            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padMode (pad), 1 }, nm ("Mode"),
                kPadModeLabels,
                static_cast<int> (PadMode::Fixed)));

            // Range bounds used only in Keyboard mode. Defaults span the full
            // MIDI keyboard so flipping a pad to Keyboard without further
            // tweaks produces an instrument that responds to every key.
            add (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padLowNote (pad), 1 }, nm ("Low Note"),
                0, 127, 0,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

            add (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padHighNote (pad), 1 }, nm ("High Note"),
                0, 127, 127,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padAttack (pad), 1 }, nm ("Attack"),
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padDecay (pad), 1 }, nm ("Decay"),
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padSustain (pad), 1 }, nm ("Sustain"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 1.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padRelease (pad), 1 }, nm ("Release"),
                NormalisableRange<float> { 0.0f, 10.0f, 0.001f, 0.4f }, 0.050f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            add (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padChokeGroup (pad), 1 }, nm ("Choke Group"),
                0, 16, 0,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return formatChokeGroup (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padStart (pad), 1 }, nm ("Start"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.000001f }, 0.0f,
                slicePointAttributes()));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padEnd (pad), 1 }, nm ("End"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.000001f }, 1.0f,
                slicePointAttributes()));

            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padReverse (pad), 1 }, nm ("Reverse"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padVelocityCurve (pad), 1 }, nm ("Velocity Curve"),
                kVelocityCurveLabels,
                static_cast<int> (VelocityCurve::Linear)));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padFilterMode (pad), 1 }, nm ("Filter Mode"),
                kFilterModeLabels,
                static_cast<int> (FilterMode::Off)));

            // Cutoff is log-skewed so half-travel sits near a musically useful
            // frequency. skew of 0.3 lands ~700 Hz at 50%, a common SVF default.
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFilterCutoff (pad), 1 }, nm ("Filter Cutoff"),
                NormalisableRange<float> { 20.0f, 20000.0f, 1.0f, 0.3f }, 20000.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("Hz")
                    .withStringFromValueFunction ([] (float v, int) { return formatHz (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFilterResonance (pad), 1 }, nm ("Filter Resonance"),
                NormalisableRange<float> { 0.1f, 10.0f, 0.01f, 0.5f }, 0.707f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatResonance (v); })));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLoopMode (pad), 1 }, nm ("Loop Mode"),
                kLoopModeLabels,
                static_cast<int> (LoopMode::Off)));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLoopStart (pad), 1 }, nm ("Loop Start"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.000001f }, 0.0f,
                slicePointAttributes()));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLoopEnd (pad), 1 }, nm ("Loop End"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.000001f }, 1.0f,
                slicePointAttributes()));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLayerMode (pad), 1 }, nm ("Layer Mode"),
                kLayerModeLabels,
                static_cast<int> (LayerMode::Velocity)));

            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padRepeatEnabled (pad), 1 }, nm ("Repeat"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padRepeatRate (pad), 1 }, nm ("Repeat Rate"),
                kRepeatRateLabels,
                static_cast<int> (NoteRepeatRate::Sixteenth)));

            // One-shot mode: when on, note-off is ignored and the voice
            // plays to the end of its sample window (or loop end, if looped).
            // The MPC default for drums - kicks, snares, claps should not
            // release their tail when the user lifts the key.
            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padOneShot (pad), 1 }, nm ("One Shot"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            // Mute and Solo. Both are simple booleans the user can flip from
            // REAPER's FX param list to dial in a beat without rewiring choke
            // groups. Solo overrides mute across pads (the standard DAW rule):
            // if any pad is soloed, only soloed pads play, regardless of the
            // mute state of other pads.
            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padMute (pad), 1 }, nm ("Mute"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padSolo (pad), 1 }, nm ("Solo"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            // Glide / portamento. Off by default so existing kits don't get
            // unexpected slides. Legato is the para-monophonic synth norm:
            // glide only when the previous voice is still sounding. Always
            // glides from the pad's most recent trigger note regardless.
            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padGlideMode (pad), 1 }, nm ("Glide Mode"),
                kGlideModeLabels,
                static_cast<int> (GlideMode::Off)));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padGlideTime (pad), 1 }, nm ("Glide Time"),
                NormalisableRange<float> { 0.0f, 2000.0f, 0.1f, 0.4f }, 100.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            // Per-pad voice cap. 1 = monophonic (a defining trap-bass setting:
            // each retrigger steals the previous voice). 8 = the global pool
            // size divided by the pad count, so every pad can reach its cap at
            // the same time without stealing from its neighbours.
            add (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padMaxVoices (pad), 1 }, nm ("Max Voices"),
                1, 8, 8,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int) { return juce::String (v); })));

            // ---- Saturation / bit-crush character chain ----
            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padSatMode (pad), 1 }, nm ("Saturation Mode"),
                kSaturationModeLabels,
                static_cast<int> (SaturationMode::Tube)));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padSatAmount (pad), 1 }, nm ("Saturation"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            // Bit depth: 16 = bypass (clean), lower = grittier. Displayed as a
            // bit count; 16 reads "Off" so the screen reader is unambiguous.
            add (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padBitDepth (pad), 1 }, nm ("Bit Depth"),
                2, 16, 16,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int)
                        { return v >= 16 ? juce::String ("Off")
                                         : juce::String (v) + " bit"; })));

            // Sample-rate reduction: divide the effective rate by this factor.
            // 1 = bypass; higher = more aliasing / lo-fi character.
            add (std::make_unique<AudioParameterInt> (
                ParameterID { param_ids::padDownsample (pad), 1 }, nm ("SR Reduce"),
                1, 64, 1,
                AudioParameterIntAttributes()
                    .withStringFromValueFunction ([] (int v, int)
                        { return v <= 1 ? juce::String ("Off")
                                        : (juce::String ("/") + juce::String (v)); })));

            // ---- Pitch envelope ----
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padPitchEnvAmount (pad), 1 }, nm ("Pitch Env Amount"),
                NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("st")
                    .withStringFromValueFunction ([] (float v, int) { return formatSemitones (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padPitchEnvTime (pad), 1 }, nm ("Pitch Env Time"),
                NormalisableRange<float> { 0.0f, 2000.0f, 0.1f, 0.4f }, 80.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            // ---- Slice cleanup: fades, loop crossfade, zero-cross snap ----
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFadeIn (pad), 1 }, nm ("Fade In"),
                NormalisableRange<float> { 0.0f, 500.0f, 0.1f, 0.5f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFadeOut (pad), 1 }, nm ("Fade Out"),
                NormalisableRange<float> { 0.0f, 500.0f, 0.1f, 0.5f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLoopXfade (pad), 1 }, nm ("Loop Crossfade"),
                NormalisableRange<float> { 0.0f, 500.0f, 0.1f, 0.5f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("ms")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 1) + " ms"; })));

            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padZeroSnap (pad), 1 }, nm ("Zero-Cross Snap"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? juce::String ("On") : juce::String ("Off"); })));

            // ---- Per-pad LFO ----
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLfoRate (pad), 1 }, nm ("LFO Rate"),
                NormalisableRange<float> { 0.01f, 20.0f, 0.01f, 0.4f }, 1.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("Hz")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 2) + " Hz"; })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLfoDepth (pad), 1 }, nm ("LFO Depth"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLfoShape (pad), 1 }, nm ("LFO Shape"),
                kLfoShapeLabels,
                static_cast<int> (LfoShape::Sine)));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLfoDest (pad), 1 }, nm ("LFO Destination"),
                kLfoDestLabels,
                static_cast<int> (LfoDest::Off)));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLfoSync (pad), 1 }, nm ("LFO Sync"),
                kLfoSyncLabels, static_cast<int> (LfoSync::Off)));

            // ---- Second LFO: same ranges as the first ----
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLfo2Rate (pad), 1 }, nm ("LFO 2 Rate"),
                NormalisableRange<float> { 0.01f, 20.0f, 0.01f, 0.4f }, 4.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("Hz")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 2) + " Hz"; })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padLfo2Depth (pad), 1 }, nm ("LFO 2 Depth"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLfo2Shape (pad), 1 }, nm ("LFO 2 Shape"),
                kLfoShapeLabels, static_cast<int> (LfoShape::Sine)));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLfo2Dest (pad), 1 }, nm ("LFO 2 Destination"),
                kLfoDestLabels, static_cast<int> (LfoDest::Off)));

            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padLfo2Sync (pad), 1 }, nm ("LFO 2 Sync"),
                kLfoSyncLabels, static_cast<int> (LfoSync::Off)));

            // ---- Mod wheel (CC 1) ----
            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padModWheelDest (pad), 1 }, nm ("Mod Wheel Destination"),
                kModDestLabels, static_cast<int> (ModDest::Off)));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padModWheelAmount (pad), 1 }, nm ("Mod Wheel Amount"),
                NormalisableRange<float> { -1.0f, 1.0f, 0.001f }, 0.5f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int)
                        { return (v > 0.0f ? "+" : "") + formatPercent (v); })));

            // ---- Velocity modulation ----
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVelToCutoff (pad), 1 }, nm ("Vel to Cutoff"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVelToPitch (pad), 1 }, nm ("Vel to Pitch"),
                NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("st")
                    .withStringFromValueFunction ([] (float v, int) { return formatSemitones (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVelToStart (pad), 1 }, nm ("Vel to Start"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            // ---- Note-repeat feel ----
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padSwing (pad), 1 }, nm ("Swing"),
                NormalisableRange<float> { 0.0f, 0.75f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padHumanize (pad), 1 }, nm ("Humanize"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            // ---- Tempo sync ----
            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padTempoSync (pad), 1 }, nm ("Tempo Sync"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? "On" : "Off"; })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padOriginalBpm (pad), 1 }, nm ("Original BPM"),
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
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padVelToGlide (pad), 1 }, nm ("Vel to Glide"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            // ---- Filter envelope ----
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvAmount (pad), 1 }, nm ("Filter Env Amount"),
                NormalisableRange<float> { -8.0f, 8.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("oct")
                    .withStringFromValueFunction ([] (float v, int)
                                                   { return juce::String (v, 2) + " oct"; })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvAttack (pad), 1 }, nm ("Filter Env Attack"),
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.001f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvDecay (pad), 1 }, nm ("Filter Env Decay"),
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.200f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvSustain (pad), 1 }, nm ("Filter Env Sustain"),
                NormalisableRange<float> { 0.0f, 1.0f, 0.001f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatPercent (v); })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFiltEnvRelease (pad), 1 }, nm ("Filter Env Release"),
                NormalisableRange<float> { 0.0f, 5.0f, 0.001f, 0.4f }, 0.100f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return formatTime (v); })));

            // ---- Stereo shaping ----
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padStereoWidth (pad), 1 }, nm ("Stereo Width"),
                NormalisableRange<float> { 0.0f, 2.0f, 0.01f }, 1.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int)
                    {
                        if (v < 0.005f)               return juce::String ("Mono");
                        if (std::abs (v - 1.0f) < 0.005f) return juce::String ("Normal");
                        return juce::String (static_cast<int> (std::round (v * 100.0f))) + "%";
                    })));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padMonoBelow (pad), 1 }, nm ("Mono Below"),
                NormalisableRange<float> { 0.0f, 500.0f, 1.0f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("Hz")
                    .withStringFromValueFunction ([] (float v, int)
                    {
                        if (v < 1.0f) return juce::String ("Off");
                        return juce::String (static_cast<int> (v)) + " Hz";
                    })));

            // ---- Time-stretch / formant ----
            add (std::make_unique<AudioParameterChoice> (
                ParameterID { param_ids::padStretchMode (pad), 1 }, nm ("Stretch Mode"),
                kStretchModeLabels, static_cast<int> (StretchMode::Repitch)));

            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padFormant (pad), 1 }, nm ("Formant"),
                NormalisableRange<float> { -12.0f, 12.0f, 0.01f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("st")
                    .withStringFromValueFunction ([] (float v, int)
                    {
                        if (std::abs (v) < 0.005f) return juce::String ("Off");
                        return formatSemitones (v);
                    })));

            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padFormantCorrect (pad), 1 },
                nm ("Formant Correct"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? "On" : "Off"; })));

            // ---- Key / scale lock ----
            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padScaleLock (pad), 1 }, nm ("Scale Lock"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int)
                                                   { return v ? "On" : "Off"; })));

            // ---- Per-layer gain trim and key mapping ----
            for (int L = 1; L <= kNumLayers; ++L)
            {
                add (std::make_unique<AudioParameterFloat> (
                    ParameterID { param_ids::padLayerGain (pad, L), 1 },
                    nm ("Layer " + juce::String (L) + " Gain"),
                    NormalisableRange<float> { -24.0f, 24.0f, 0.1f }, 0.0f,
                    AudioParameterFloatAttributes()
                        .withLabel ("dB")
                        .withStringFromValueFunction ([] (float v, int)
                                                       { return formatDb (v); })));

                // Key-zone mapping. Defaults span the whole keyboard with a
                // root of C3, so switching Layer Mode to Key Zones without
                // touching these behaves like the single-sample case.
                add (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerLowNote (pad, L), 1 },
                    nm ("Layer " + juce::String (L) + " Low Note"),
                    0, 127, 0,
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

                add (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerHighNote (pad, L), 1 },
                    nm ("Layer " + juce::String (L) + " High Note"),
                    0, 127, 127,
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

                add (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerRootNote (pad, L), 1 },
                    nm ("Layer " + juce::String (L) + " Root Note"),
                    0, 127, 60,
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int) { return formatNote (v); })));

                // Velocity range, used in Velocity layer mode. Overlapping
                // ranges play together, so two layers can be stacked.
                add (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerVelLow (pad, L), 1 },
                    nm ("Layer " + juce::String (L) + " Vel Low"),
                    0, 127, defaultLayerVelLow (L),
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int) { return juce::String (v); })));

                add (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerVelHigh (pad, L), 1 },
                    nm ("Layer " + juce::String (L) + " Vel High"),
                    0, 127, defaultLayerVelHigh (L),
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int) { return juce::String (v); })));

                // Press: plays on note-on. Release: plays on note-off, with the
                // velocity the key was struck with, for release noises and tails.
                add (std::make_unique<AudioParameterChoice> (
                    ParameterID { param_ids::padLayerTrigger (pad, L), 1 },
                    nm ("Layer " + juce::String (L) + " Trigger"),
                    kLayerTriggerLabels, 0));

                // Per-layer tuning, so samples recorded slightly apart (or a
                // zone whose root is not quite on a note) play in tune.
                add (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerTranspose (pad, L), 1 },
                    nm ("Layer " + juce::String (L) + " Transpose"),
                    -24, 24, 0,
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int)
                            { return (v > 0 ? "+" : "") + juce::String (v) + " st"; })));

                add (std::make_unique<AudioParameterInt> (
                    ParameterID { param_ids::padLayerFine (pad, L), 1 },
                    nm ("Layer " + juce::String (L) + " Fine Tune"),
                    -100, 100, 0,
                    AudioParameterIntAttributes()
                        .withStringFromValueFunction ([] (int v, int)
                            { return (v > 0 ? "+" : "") + juce::String (v) + " cents"; })));
            }

            // ---- Speed and Keep Length (rendered, like Stretch) ----
            //
            // Speed: +100% plays twice as fast, -100% half as fast, pitch
            // unchanged. The render happens in the background, as for Stretch.
            add (std::make_unique<AudioParameterFloat> (
                ParameterID { param_ids::padSpeed (pad), 1 }, nm ("Speed"),
                NormalisableRange<float> { -100.0f, 100.0f, 1.0f }, 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("%")
                    .withStringFromValueFunction ([] (float v, int)
                    {
                        if (std::abs (v) < 0.5f) return juce::String ("Off");
                        return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + "%, "
                             + juce::String (std::pow (2.0f, v / 100.0f), 2) + "x speed";
                    })));

            // Keep Length: a note played away from the pad's root lasts as long
            // as the sample does at the root, instead of getting shorter as it
            // goes up. Rendered per semitone, like Formant Correct.
            add (std::make_unique<SnappedBoolParameter> (
                ParameterID { param_ids::padKeepLength (pad), 1 }, nm ("Keep Length"), false,
                AudioParameterBoolAttributes()
                    .withStringFromValueFunction ([] (bool v, int) { return v ? "On" : "Off"; })));

            // File each parameter under its section, in spec order: Pad 9 /
            // Filter / P9 Filter Cutoff. Hosts that show VST3 units (and
            // OSARA's parameter dialog, which builds a tree from them) let the
            // user open one section instead of arrowing through 80 entries.
            auto group = std::make_unique<AudioProcessorParameterGroup> (
                "pad_" + String (pad), "Pad " + String (pad), "|");

            std::vector<std::pair<juce::String, std::unique_ptr<AudioProcessorParameterGroup>>> subgroups;
            auto subgroupFor = [&subgroups, pad] (const juce::String& section) -> AudioProcessorParameterGroup&
            {
                for (auto& [name, g] : subgroups)
                    if (name == section)
                        return *g;
                subgroups.emplace_back (section, std::make_unique<AudioProcessorParameterGroup> (
                    "pad_" + String (pad) + "_" + section.toLowerCase().removeCharacters (" "),
                    section, "|"));
                return *subgroups.back().second;
            };

            for (const auto& spec : padParamSpecs())
            {
                auto it = made.find (spec.idFor (pad));
                if (it == made.end())
                {
                    jassertfalse;   // a spec with no parameter: the two lists drifted
                    continue;
                }
                subgroupFor (spec.section).addChild (std::move (it->second));
                made.erase (it);
            }

            // Anything made but not listed in padParamSpecs() would be missing
            // from the Focus group; the parity tests catch it, this keeps it
            // reachable meanwhile.
            jassert (made.empty());
            for (auto& [id, param] : made)
                subgroupFor ("Other").addChild (std::move (param));

            for (auto& [name, g] : subgroups)
                group->addChild (std::move (g));

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
          state (*this, &undoManager, "STATE",
                 makeParameterLayout ([this] (int pad) { return getPadReferenceSeconds (pad); }))
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

        vst3Extensions = makeReaperVst3Extensions (reaperApi);

        // Poll for stretch/formant work ten times a second, so a change starts
        // rendering almost at once. Cheap (a handful of atomic loads per pad)
        // and it picks up host tempo changes, which no parameter callback
        // would report.
        startTimer (100);

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
        renderPool.removeAllJobs (true, 5000);
        *alive = false;   // drop any completion still queued on the message thread

        for (auto* p : getParameters())
            p->removeListener (this);
    }

    juce::AudioProcessorValueTreeState::ParameterLayout
    ASamplerProcessor::makeParameterLayout (const PadLengthFn& padSeconds)
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { param_ids::masterGain, 1 }, "Master Gain",
            NormalisableRange<float> { -60.0f, 12.0f, 0.01f }, 0.0f,
            AudioParameterFloatAttributes()
                .withLabel ("dB")
                .withStringFromValueFunction ([] (float v, int) { return formatDb (v); })));

        // Global "Chop Count" choice — used by chopAcrossPads. Default is 16,
        // matching the most common MPC workflow of chopping a 1-bar loop into
        // 16ths; 32 spreads a loop over both banks.
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { param_ids::chopCount, 1 }, "Chop Count",
            kChopCountLabels,
            kDefaultChopIndex));

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
        layout.add (std::make_unique<SnappedBoolParameter> (
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

        // "Audition On Edit" — play the pad when its Start or End settles after
        // an edit, so a slice boundary can be placed by ear. On by default:
        // without sight it is the only feedback that a boundary moved. Never
        // fires while the transport runs, so automation is unaffected.
        layout.add (std::make_unique<SnappedBoolParameter> (
            ParameterID { param_ids::auditionOnEdit, 1 }, "Audition On Edit", true,
            AudioParameterBoolAttributes()
                .withStringFromValueFunction ([] (bool v, int)
                                               { return v ? juce::String ("On") : juce::String ("Off"); })));

        // Velocity of pads played from the computer keyboard. 100 is a firm
        // hit that leaves room above it, the usual default for typing keys.
        layout.add (std::make_unique<AudioParameterInt> (
            ParameterID { param_ids::keyVelocity, 1 }, "Computer Key Velocity",
            1, 127, 100,
            AudioParameterIntAttributes()
                .withStringFromValueFunction ([] (int v, int) { return juce::String (v); })));

        // Trim leading silence whenever a sample is loaded onto a pad's first
        // layer. Off by default: it moves Start, which a user may not expect.
        layout.add (std::make_unique<SnappedBoolParameter> (
            ParameterID { param_ids::trimOnLoad, 1 }, "Trim Silence On Load", false,
            AudioParameterBoolAttributes()
                .withStringFromValueFunction ([] (bool v, int)
                                               { return v ? juce::String ("On") : juce::String ("Off"); })));

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

            // Same sections as the pad groups, so the two read alike.
            std::vector<std::pair<juce::String, std::unique_ptr<AudioProcessorParameterGroup>>> subgroups;
            for (const auto& spec : padParamSpecs())
            {
                const juce::String section (spec.section);
                auto it = std::find_if (subgroups.begin(), subgroups.end(),
                                        [&section] (const auto& s) { return s.first == section; });
                if (it == subgroups.end())
                {
                    subgroups.emplace_back (section, std::make_unique<AudioProcessorParameterGroup> (
                        "focus_" + section.toLowerCase().removeCharacters (" "), section, "|"));
                    it = std::prev (subgroups.end());
                }

                it->second->addChild (std::make_unique<FocusProxyParameter> (
                    focusProxyIdFor (spec), "Focus " + juce::String (spec.name), kNumPads));
            }

            for (auto& [name, g] : subgroups)
                focusGroup->addChild (std::move (g));

            layout.add (std::move (focusGroup));
        }

        for (int p = 1; p <= kNumPads; ++p)
            layout.add (makePadGroup (p, padSeconds));

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
        auditionOnEditParam = state.getRawParameterValue (param_ids::auditionOnEdit);
        keyVelocityParam    = state.getRawParameterValue (param_ids::keyVelocity);
        trimOnLoadParam     = state.getRawParameterValue (param_ids::trimOnLoad);

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
            padParams[i].lfoSync        = state.getRawParameterValue (param_ids::padLfoSync (p));
            padParams[i].lfo2Rate       = state.getRawParameterValue (param_ids::padLfo2Rate (p));
            padParams[i].lfo2Depth      = state.getRawParameterValue (param_ids::padLfo2Depth (p));
            padParams[i].lfo2Shape      = state.getRawParameterValue (param_ids::padLfo2Shape (p));
            padParams[i].lfo2Dest       = state.getRawParameterValue (param_ids::padLfo2Dest (p));
            padParams[i].lfo2Sync       = state.getRawParameterValue (param_ids::padLfo2Sync (p));
            padParams[i].modWheelDest   = state.getRawParameterValue (param_ids::padModWheelDest (p));
            padParams[i].modWheelAmount = state.getRawParameterValue (param_ids::padModWheelAmount (p));

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
                padParams[i].layerVelLow[(std::size_t) L] =
                    state.getRawParameterValue (param_ids::padLayerVelLow (p, L + 1));
                padParams[i].layerVelHigh[(std::size_t) L] =
                    state.getRawParameterValue (param_ids::padLayerVelHigh (p, L + 1));
                padParams[i].layerTrigger[(std::size_t) L] =
                    state.getRawParameterValue (param_ids::padLayerTrigger (p, L + 1));
                padParams[i].layerTranspose[(std::size_t) L] =
                    state.getRawParameterValue (param_ids::padLayerTranspose (p, L + 1));
                padParams[i].layerFine[(std::size_t) L] =
                    state.getRawParameterValue (param_ids::padLayerFine (p, L + 1));
            }
            padParams[i].speed      = state.getRawParameterValue (param_ids::padSpeed (p));
            padParams[i].keepLength = state.getRawParameterValue (param_ids::padKeepLength (p));
        }

        scaleKeyParam  = state.getRawParameterValue (param_ids::scaleKey);
        scaleTypeParam = state.getRawParameterValue (param_ids::scaleType);

        focusedPadParam = state.getRawParameterValue (param_ids::focusedPad);

        // Second phase of the Focus proxies' setup: the proxies were created
        // in the parameter layout, but their targets only exist once the APVTS
        // has finished constructing. Resolve every target now so nothing on a
        // hot path ever does a string lookup.
        //
        // Proxies are matched to specs by ID, not by position: the layout
        // files them into section sub-groups, so their order in
        // getParameters() is not the spec order.
        std::map<juce::String, FocusProxyParameter*> proxiesById;
        for (auto* p : getParameters())
            if (auto* proxy = dynamic_cast<FocusProxyParameter*> (p))
                proxiesById[proxy->getParameterID()] = proxy;

        focusProxies.clear();
        focusProxies.reserve (padParamSpecs().size());

        for (const auto& spec : padParamSpecs())
        {
            auto it = proxiesById.find (focusProxyIdFor (spec));
            if (it == proxiesById.end())
            {
                jassertfalse;
                continue;
            }

            std::vector<juce::RangedAudioParameter*> targets;
            targets.reserve ((std::size_t) kNumPads);
            for (int pad = 1; pad <= kNumPads; ++pad)
                targets.push_back (state.getParameter (spec.idFor (pad)));

            it->second->setTargets (std::move (targets), focusedPadParam);
            focusProxies.push_back (it->second);
        }

        jassert (focusProxies.size() == padParamSpecs().size());

        focusedPadParamIndex = -1;
        if (auto* fp = state.getParameter (param_ids::focusedPad))
            focusedPadParamIndex = fp->getParameterIndex();

        // Parameter index -> (pad, Start/End), for audition-on-edit. Only the
        // real per-pad parameters: a Focus proxy edit forwards to one of these
        // and is picked up there.
        sliceEditLookup.assign ((std::size_t) getParameters().size(), -1);
        for (int i = 0; i < kNumPads; ++i)
        {
            if (auto* s = state.getParameter (param_ids::padStart (i + 1)))
                sliceEditLookup[(std::size_t) s->getParameterIndex()] = i * 2;
            if (auto* e = state.getParameter (param_ids::padEnd (i + 1)))
                sliceEditLookup[(std::size_t) e->getParameterIndex()] = i * 2 + 1;
        }
        for (auto& s : pendingAuditionStart)  s.store (-1.0f);
        auditionStartOverride.fill (-1.0f);
    }

    void ASamplerProcessor::setFocusedPad (int padIndex)
    {
        auto* param = state.getParameter (param_ids::focusedPad);
        if (param == nullptr || padIndex < 0 || padIndex >= kNumPads)
            return;

        const float plain = static_cast<float> (padIndex + 1);
        if (juce::roundToInt (param->convertFrom0to1 (param->getValue())) == padIndex + 1)
            return;

        // Write the tree's value first, without an UndoManager. APVTS later
        // flushes the parameter into the tree *with* undo recording, but only
        // when the two differ, so matching them here keeps the change out of
        // the undo history.
        auto node = state.state.getChildWithProperty ("id", juce::String (param_ids::focusedPad));
        if (node.isValid())
            node.setProperty ("value", plain, nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (plain));
    }

    bool ASamplerProcessor::isPadMuted (int padIndex) const
    {
        return juce::isPositiveAndBelow (padIndex, kNumPads)
            && padParams[(std::size_t) padIndex].mute->load() >= 0.5f;
    }

    bool ASamplerProcessor::isPadSoloed (int padIndex) const
    {
        return juce::isPositiveAndBelow (padIndex, kNumPads)
            && padParams[(std::size_t) padIndex].solo->load() >= 0.5f;
    }

    int ASamplerProcessor::getCurrentFocusedPad() const
    {
        if (focusedPadParam == nullptr) return 0;
        // Parameter is 1-based ("Pad 1".."Pad 32"); expose a 0-based index.
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

        // Forget the last-triggered note per pad so the first note after a
        // transport stop never glides.
        lastTriggerNote.fill (-1);
        lastTriggerPitchSt.fill (0.0f);
    }

    int ASamplerProcessor::getActiveVoiceCount() const
    {
        return voicePool.countActive();
    }

    bool ASamplerProcessor::isPadSounding (int padIndex) const
    {
        return juce::isPositiveAndBelow (padIndex, kNumPads) && voicePool.hasActivePad (padIndex);
    }

    double ASamplerProcessor::getHostBpm() const
    {
        // The play head is only guaranteed valid inside processBlock, but the
        // timer, Describe and Bounce need the tempo on the message thread. So
        // processBlock caches it, and everything else reads the cache.
        if (const double cached = cachedHostBpm.load (std::memory_order_relaxed); cached > 0.0)
            return cached;

        // Nothing cached yet (no block processed): ask directly. If there's
        // no transport info (offline render, transport stopped in some hosts),
        // fall back to 120 BPM so tempo-dependent features still behave
        // predictably.
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
                    if (! padRespondsToNote (p, note)) continue;
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
        const double bpm     = getHostBpm();
        live.lfoRateHz       = lfoRateFor (p.lfoRate->load(),
                                           static_cast<LfoSync> (juce::roundToInt (p.lfoSync->load())), bpm);
        live.lfoDepth        = p.lfoDepth->load();
        live.lfo2RateHz      = lfoRateFor (p.lfo2Rate->load(),
                                           static_cast<LfoSync> (juce::roundToInt (p.lfo2Sync->load())), bpm);
        live.lfo2Depth       = p.lfo2Depth->load();
        live.modWheelAmount  = p.modWheelAmount->load();
        live.stereoWidth     = p.stereoWidth->load();
        live.monoBelowHz     = p.monoBelow->load();
        live.filterEnvOctaves = p.filtEnvAmount->load();
        return live;
    }

    bool ASamplerProcessor::layerIsLoaded (int padIndex, int layerIndex) const noexcept
    {
        const auto s = pads[(std::size_t) padIndex].layers[(std::size_t) layerIndex].sample.load();
        return s != nullptr && s.get() != defaultSample.get();
    }

    int ASamplerProcessor::selectLayerForTrigger (int padIndex, int midiNote, float velocity) const
    {
        int layers[kNumLayers];
        return selectLayersForTrigger (padIndex, midiNote, velocity, false, layers) > 0 ? layers[0] : 0;
    }

    int ASamplerProcessor::selectLayersForTrigger (int padIndex, int midiNote, float velocity,
                                                   bool onRelease, int* out) const
    {
        const auto& pp = padParams[(std::size_t) padIndex];

        // Previewing a candidate: the pad plays just that, on the edit layer.
        if (previewOverride[(std::size_t) padIndex].load() != nullptr)
        {
            if (onRelease)
                return 0;
            out[0] = getCurrentEditLayer();
            return 1;
        }

        // The loaded layers for this edge. Pointer compares only, so this is
        // safe against a load landing on the message thread mid-scan.
        int candidates[kNumLayers];
        int count = 0;
        bool anyLoaded = false;
        for (int L = 0; L < kNumLayers; ++L)
        {
            if (! layerIsLoaded (padIndex, L))
                continue;
            anyLoaded = true;
            const bool isRelease = juce::roundToInt (pp.layerTrigger[(std::size_t) L]->load()) == 1;
            if (isRelease == onRelease)
                candidates[count++] = L;
        }

        if (count == 0)
        {
            // An untouched pad still speaks with its built-in tone on press.
            if (! anyLoaded && ! onRelease)
            {
                out[0] = 0;
                return 1;
            }
            return 0;
        }

        const auto mode = static_cast<LayerMode> (juce::roundToInt (pp.layerMode->load()));

        switch (mode)
        {
            case LayerMode::Velocity:
            {
                // Every layer whose range holds the hit plays: one layer per
                // band switches by velocity, overlapping bands stack.
                const int v127 = juce::jlimit (0, 127, juce::roundToInt (velocity * 127.0f));
                int n = 0;
                for (int i = 0; i < count; ++i)
                {
                    const int L  = candidates[i];
                    const int a  = juce::roundToInt (pp.layerVelLow [(std::size_t) L]->load());
                    const int b  = juce::roundToInt (pp.layerVelHigh[(std::size_t) L]->load());
                    if (v127 >= juce::jmin (a, b) && v127 <= juce::jmax (a, b))
                        out[n++] = L;
                }
                if (n > 0)
                    return n;

                // No range holds it (a gap between bands, or only some layers
                // loaded): use the layer whose range is nearest, preferring
                // the one above on a tie, so a hit is never silent.
                int best = candidates[0];
                int bestDist = 1000;
                for (int i = 0; i < count; ++i)
                {
                    const int L  = candidates[i];
                    const int a  = juce::roundToInt (pp.layerVelLow [(std::size_t) L]->load());
                    const int b  = juce::roundToInt (pp.layerVelHigh[(std::size_t) L]->load());
                    const int lo = juce::jmin (a, b), hi = juce::jmax (a, b);
                    const int dist = v127 < lo ? lo - v127 : v127 - hi;
                    if (dist < bestDist || (dist == bestDist && v127 < lo))
                    {
                        bestDist = dist;
                        best = L;
                    }
                }
                out[0] = best;
                return 1;
            }

            case LayerMode::RoundRobin:
            {
                // Cycle through loaded layers in index order. Counter wraps via
                // modulo; reloading a layer mid-rotation is tolerated.
                const int next = roundRobinIndex[(std::size_t) padIndex]
                                     .fetch_add (1, std::memory_order_relaxed);
                out[0] = candidates[((next % count) + count) % count];
                return 1;
            }

            case LayerMode::Random:
                out[0] = candidates[juce::Random::getSystemRandom().nextInt (count)];
                return 1;

            case LayerMode::KeyZones:
            {
                // Pick the loaded layer whose [low, high] contains the note.
                // This is what turns the layers into a multisampled
                // instrument rather than one sample stretched across the range.
                for (int i = 0; i < count; ++i)
                {
                    const int L  = candidates[i];
                    const int lo = juce::roundToInt (pp.layerLowNote [(std::size_t) L]->load());
                    const int hi = juce::roundToInt (pp.layerHighNote[(std::size_t) L]->load());
                    if (midiNote >= juce::jmin (lo, hi) && midiNote <= juce::jmax (lo, hi))
                    {
                        out[0] = L;
                        return 1;
                    }
                }

                // Note falls outside every zone: use the loaded layer whose
                // root is closest, so the pad still speaks instead of going
                // silent on a key the user pressed.
                int best = candidates[0];
                int bestDist = 1000;
                for (int i = 0; i < count; ++i)
                {
                    const int L = candidates[i];
                    const int root = juce::roundToInt (pp.layerRootNote[(std::size_t) L]->load());
                    const int dist = std::abs (midiNote - root);
                    if (dist < bestDist) { bestDist = dist; best = L; }
                }
                out[0] = best;
                return 1;
            }
        }

        out[0] = candidates[0];
        return 1;
    }

    ASamplerProcessor::TriggerPlan
    ASamplerProcessor::planTrigger (int padIndex, int midiNote, float velocity,
                                    int selectedLayer, bool liveTrigger)
    {
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
        cfg.pitchBendSemitones = liveTrigger ? currentPitchBendSemitones : 0.0f;
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
        const double bpm = getHostBpm();
        const auto& mp   = padParams[padIndex];
        cfg.lfoRateHz = lfoRateFor (mp.lfoRate->load(),
                                    static_cast<LfoSync> (juce::roundToInt (mp.lfoSync->load())), bpm);
        cfg.lfoDepth  = mp.lfoDepth->load();

        cfg.lfo2Shape  = static_cast<LfoShape> (juce::roundToInt (mp.lfo2Shape->load()));
        cfg.lfo2Dest   = static_cast<LfoDest>  (juce::roundToInt (mp.lfo2Dest->load()));
        cfg.lfo2RateHz = lfoRateFor (mp.lfo2Rate->load(),
                                     static_cast<LfoSync> (juce::roundToInt (mp.lfo2Sync->load())), bpm);
        cfg.lfo2Depth  = mp.lfo2Depth->load();

        cfg.modWheelDest   = static_cast<ModDest> (juce::roundToInt (mp.modWheelDest->load()));
        cfg.modWheelAmount = mp.modWheelAmount->load();
        cfg.modWheel       = currentModWheel.load (std::memory_order_relaxed);

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

        // An audition of a slice-point edit starts where it was asked to
        // (e.g. just before End), once.
        if (auto& over = auditionStartOverride[(std::size_t) padIndex]; liveTrigger && over >= 0.0f)
        {
            cfg.startNorm = juce::jlimit (0.0f, juce::jmax (0.0f, cfg.endNorm - 0.001f), over);
            over = -1.0f;
        }

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

        const bool legatoOk = liveTrigger && voicePool.hasActivePad (padIndex);
        const bool doGlide  = liveTrigger
                           && lastNote >= 0
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

        if (liveTrigger)
        {
            lastTriggerNote   [(std::size_t) padIndex] = midiNote;
            lastTriggerPitchSt[(std::size_t) padIndex] = currentPitch;
        }

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

        auto plan = planLayer (padIndex, selectedLayer, cfg, triggerNote);
        plan.baseCfg = cfg;
        return plan;
    }

    ASamplerProcessor::TriggerPlan
    ASamplerProcessor::planLayer (int padIndex, int layerIndex, VoiceConfig cfg, int triggerNote) const
    {
        const auto& pp = padParams[(std::size_t) padIndex];
        const auto  L  = (std::size_t) layerIndex;

        // Per-layer gain trim. Folded into both the final gain and the
        // per-hit scale, so a live volume push preserves it the same way
        // it preserves velocity.
        if (const float layerDb = pp.layerGain[L]->load(); layerDb != 0.0f)
        {
            const float layerLin = juce::Decibels::decibelsToGain (layerDb, -60.0f);
            cfg.gainLin      *= layerLin;
            cfg.hitGainScale *= layerLin;
        }

        // Per-layer tuning rides on top of everything else, after the glide
        // snapshot, so a stacked layer tuned an octave up doesn't read as a
        // note-to-note slide.
        cfg.pitchSemitones += pp.layerTranspose[L]->load() + 0.01f * pp.layerFine[L]->load();

        // Sample selection, most specific first:
        //   1. the per-note variant (Formant Correct / Keep Length) for this
        //      exact pitch offset,
        //   2. the pad's single stretched / formant-shifted render,
        //   3. the raw sample.
        // All three already carry the pad's settings, so playback stays a
        // plain resample either way.
        const auto& layerSlot = pads[(std::size_t) padIndex].layers[L];
        std::shared_ptr<const Sample> sample;

        // A previewed candidate replaces the edit layer, raw: it has no
        // stretch render or per-note table of its own yet.
        if (layerIndex == getCurrentEditLayer())
            sample = previewOverride[(std::size_t) padIndex].load();

        if (sample != nullptr) {}
        else if (auto table = layerSlot.formantTable.load())
        {
            // cfg.pitchSemitones is the voice's whole pitch offset by this
            // point — note interval, Pitch parameter, tempo-sync repitch,
            // Vel->Pitch and layer tuning — so rounding it gives the variant
            // that matches everything at once. Residual error is at most half
            // a semitone, which is inaudible as formant or length.
            const int slotIndex = juce::roundToInt (cfg.pitchSemitones)
                                + kMaxFormantOffset;

            if (slotIndex >= 0 && slotIndex < kFormantVariantSlots)
                sample = table->variants[(std::size_t) slotIndex];
        }

        if (sample == nullptr)
            sample = layerSlot.processed.load();
        if (sample == nullptr)
            sample = layerSlot.sample.load();

        TriggerPlan plan;
        plan.cfg         = cfg;
        plan.triggerNote = triggerNote;
        plan.sample      = std::move (sample);
        return plan;
    }

    void ASamplerProcessor::handleNoteOn (int midiNote, float velocity)
    {
        std::array<int, kNumPads> chokeTargets {};
        int chokeCount = 0;

        std::array<int, kNumPads> trigger {};
        int triggerCount = 0;

        for (int i = 0; i < kNumPads; ++i)
        {
            // The pitch shift relative to a Keyboard pad's root is applied in
            // the trigger loop below.
            if (! padRespondsToNote (i, midiNote))
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

            if (juce::isPositiveAndBelow (midiNote, 128))
                noteOnVelocity[(std::size_t) padIndex][(std::size_t) midiNote] = velocity;

            // Layer selection happens first: in Key Zones mode the chosen
            // layer supplies the root note that the pitch offset is measured
            // from, so it has to be known before pitch is computed.
            int layers[kNumLayers];
            const int numLayers = selectLayersForTrigger (padIndex, midiNote, velocity, false, layers);
            if (numLayers == 0)
                continue;   // only release layers loaded: they play at note-off

            // Stacked layers share one voice budget per layer, so a mono pad
            // still holds all of its stacked layers rather than stealing
            // between them.
            const int padCap = juce::roundToInt (padParams[padIndex].maxVoices->load()) * numLayers;

            auto plan = planTrigger (padIndex, midiNote, velocity, layers[0], true);
            const auto base = plan.baseCfg;
            const int  note = plan.triggerNote;
            voicePool.triggerPad (padIndex, std::move (plan.sample), note, plan.cfg, padCap);

            for (int k = 1; k < numLayers; ++k)
            {
                auto extra = planLayer (padIndex, layers[k], base, note);
                voicePool.triggerPad (padIndex, std::move (extra.sample), note, extra.cfg, padCap);
            }
        }
    }

    void ASamplerProcessor::triggerReleaseLayers (int padIndex, int midiNote)
    {
        const float velocity = juce::isPositiveAndBelow (midiNote, 128)
                                 ? noteOnVelocity[(std::size_t) padIndex][(std::size_t) midiNote]
                                 : 0.8f;

        int layers[kNumLayers];
        const int numLayers = selectLayersForTrigger (padIndex, midiNote, velocity, true, layers);
        for (int k = 0; k < numLayers; ++k)
        {
            // Not a live trigger: a release sound must not move the glide
            // history the next note slides from.
            auto plan = planTrigger (padIndex, midiNote, velocity, layers[k], false);
            plan.cfg.pitchBendSemitones = currentPitchBendSemitones;

            // Nothing will send this voice a note-off (the key is already up),
            // so it plays through to the end of the sample; a loop would never
            // end.
            plan.cfg.loopMode = LoopMode::Off;
            voicePool.triggerPad (padIndex, std::move (plan.sample), plan.triggerNote, plan.cfg,
                                  juce::roundToInt (padParams[(std::size_t) padIndex].maxVoices->load()) + 1);
        }
    }

    bool ASamplerProcessor::padHoldsLoadedSample (int padIndex) const noexcept
    {
        // A candidate being previewed counts: the user is trying it on this
        // pad and must hear it there.
        if (previewOverride[(std::size_t) padIndex].load() != nullptr)
            return true;

        const auto* builtIn = defaultSample.get();
        for (const auto& layer : pads[(std::size_t) padIndex].layers)
        {
            const auto s = layer.sample.load();
            if (s != nullptr && s.get() != builtIn)
                return true;
        }
        return false;
    }

    bool ASamplerProcessor::keyboardPadCovers (int midiNote) const noexcept
    {
        for (const auto& pp : padParams)
            if (static_cast<PadMode> (juce::roundToInt (pp.mode->load())) == PadMode::Keyboard
                && midiNote >= juce::roundToInt (pp.lowNote->load())
                && midiNote <= juce::roundToInt (pp.highNote->load()))
                return true;
        return false;
    }

    bool ASamplerProcessor::padRespondsToNote (int padIndex, int midiNote) const noexcept
    {
        const auto& pp = padParams[(std::size_t) padIndex];
        const auto padMode = static_cast<PadMode> (juce::roundToInt (pp.mode->load()));

        if (padMode != PadMode::Keyboard)
        {
            if (juce::roundToInt (pp.note->load()) != midiNote)
                return false;

            // An untouched pad shouldn't sound its built-in tone on top of a
            // Keyboard pad playing the same note. With 32 pads the default
            // note layout reaches well into the keyboard, so this is the
            // common case, not an edge one.
            return padHoldsLoadedSample (padIndex) || ! keyboardPadCovers (midiNote);
        }

        const int lo = juce::roundToInt (pp.lowNote->load());
        const int hi = juce::roundToInt (pp.highNote->load());
        if (midiNote < lo || midiNote > hi)
            return false;

        // A note a loaded Fixed pad is assigned to belongs to that pad.
        // Without this a Keyboard pad left at its full default range plays on
        // top of every drum in the kit, which reads as a broken pad when you
        // can't see why.
        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& other = padParams[(std::size_t) p];
            if (static_cast<PadMode> (juce::roundToInt (other.mode->load())) != PadMode::Keyboard
                && juce::roundToInt (other.note->load()) == midiNote
                && padHoldsLoadedSample (p))
                return false;
        }
        return true;
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
            if (! padRespondsToNote (i, midiNote)) continue;
            const auto padMode = static_cast<PadMode> (
                                     juce::roundToInt (padParams[i].mode->load()));

            // One-shot pads ignore note-off — the voice runs to the end of
            // its sample window (or loop boundary). This matches the MPC
            // default for drum hits: a kick doesn't truncate when you lift.
            const bool oneShot = padParams[i].oneShot != nullptr
                              && padParams[i].oneShot->load() >= 0.5f;

            // In Keyboard mode a pad can hold simultaneous voices for several
            // incoming notes; release only the voices triggered by this note.
            // In Fixed mode the pad has at most one trigger note, so releasing
            // every voice on the pad is equivalent (and stays simple).
            if (! oneShot)
            {
                if (padMode == PadMode::Keyboard)
                    voicePool.releasePadNote (i, midiNote);
                else
                    voicePool.releasePad (i);
            }

            // Release-trigger layers start now, after the release above so it
            // doesn't catch them too. One-shot or not: a release sample is
            // what lifting the key sounds like.
            triggerReleaseLayers (i, midiNote);
        }
    }

    void ASamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midi)
    {
        juce::ScopedNoDenormals noDenormals;
        buffer.clear();

        // Note learn: the first note-on while armed is captured (the timer
        // applies it) and swallowed with its note-off, so it doesn't play.
        if (noteLearnPad.load (std::memory_order_relaxed) >= 0 || noteLearnSwallowOff >= 0)
        {
            juce::MidiBuffer filtered;
            for (const auto meta : midi)
            {
                const auto msg = meta.getMessage();
                if (msg.isNoteOn() && noteLearnPad.load (std::memory_order_relaxed) >= 0
                    && learnedNote.load (std::memory_order_relaxed) < 0)
                {
                    learnedNote.store (msg.getNoteNumber(), std::memory_order_release);
                    noteLearnSwallowOff = msg.getNoteNumber();
                    continue;
                }
                if (msg.isNoteOff() && msg.getNoteNumber() == noteLearnSwallowOff)
                {
                    noteLearnSwallowOff = -1;
                    continue;
                }
                filtered.addEvent (msg, meta.samplePosition);
            }
            midi.swapWith (filtered);
        }

        // Pads played from the computer keyboard. They join the host's MIDI
        // (so note repeat, choke groups and layers treat them like any note)
        // and are what this block sends out.
        juce::MidiBuffer liveNotes;
        {
            const float keyVel = keyVelocityParam != nullptr
                                   ? juce::jlimit (1.0f, 127.0f, keyVelocityParam->load()) / 127.0f
                                   : 100.0f / 127.0f;
            int start1, size1, start2, size2;
            liveNoteFifo.prepareToRead (liveNoteFifo.getNumReady(), start1, size1, start2, size2);
            auto take = [&] (int start, int size)
            {
                for (int i = 0; i < size; ++i)
                {
                    const int v    = liveNoteData[(std::size_t) (start + i)];
                    const int note = v & 0x7f;
                    liveNotes.addEvent ((v & 0x100) != 0 ? juce::MidiMessage::noteOn (1, note, keyVel)
                                                         : juce::MidiMessage::noteOff (1, note), 0);
                }
            };
            take (start1, size1);
            take (start2, size2);
            liveNoteFifo.finishedRead (size1 + size2);
            if (! liveNotes.isEmpty())
                midi.addEvents (liveNotes, 0, -1, 0);
        }

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

        // Audition-on-edit must never fire under automation, so it needs to
        // know whether the host is playing.
        {
            bool playing = false;
            if (auto* head = getPlayHead())
                if (auto pos = head->getPosition())
                {
                    playing = pos->getIsPlaying();
                    if (auto b = pos->getBpm(); b && *b > 0.0)
                        cachedHostBpm.store (*b, std::memory_order_relaxed);
                }
            transportPlaying.store (playing, std::memory_order_relaxed);
        }

        // Audition requests posted by the editor. Consumed here so voices are
        // only ever started on the audio thread. Exchange-to-clear means a
        // request fires exactly once even if two blocks race.
        const double hostRate = getSampleRate() > 0.0 ? getSampleRate() : 44100.0;
        for (int p = 0; p < kNumPads; ++p)
        {
            const int encoded = pendingAudition[(std::size_t) p]
                                    .exchange (0, std::memory_order_acq_rel);
            if (encoded > 0)
            {
                const float velocity = static_cast<float> (encoded) / 1000.0f;

                // Fire the note this pad actually listens for, so audition takes
                // exactly the same path as a real hit (choke groups, layers, glide).
                // In Keyboard mode that is the root, which plays unpitched.
                const int note = juce::roundToInt (padParams[p].note->load());
                auditionStartOverride[(std::size_t) p] = pendingAuditionStart[(std::size_t) p].load();
                combinedMidi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), 0);

                // One-shot pads ignore note-off. The rest are held for the
                // length of what they play, then released: releasing at the end
                // of this block would cut every audition down to the release
                // time, too short to judge a sample by.
                if (padParams[p].oneShot->load() < 0.5f)
                {
                    auditionNote[(std::size_t) p] = note;
                    auditionHoldRemaining[(std::size_t) p] = juce::jmax (1, juce::roundToInt (
                        pendingAuditionHoldMs[(std::size_t) p].load() * 0.001 * hostRate));
                }
                else
                {
                    auditionHoldRemaining[(std::size_t) p] = 0;
                }
            }

            auto& remaining = auditionHoldRemaining[(std::size_t) p];
            if (remaining > 0)
            {
                if (remaining < numSamples)
                {
                    combinedMidi.addEvent (juce::MidiMessage::noteOff (1, auditionNote[(std::size_t) p]),
                                           remaining);
                    remaining = 0;
                }
                else
                {
                    remaining -= numSamples;
                }
            }
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
            else if (msg.isControllerOfType (1))
            {
                const float wheel = static_cast<float> (msg.getControllerValue()) / 127.0f;
                currentModWheel.store (wheel, std::memory_order_relaxed);
                voicePool.setModWheel (wheel);
            }
        }

        if (currentSample < numSamples)
            voicePool.renderAdding (destByPad.data(), currentSample,
                                    numSamples - currentSample);

        // Master gain affects only Main. Per-pad outs are post-pad-fx but
        // pre-master, matching the standard MPC "send to external processing"
        // convention.
        const auto gainLin = juce::Decibels::decibelsToGain (masterGainParam->load(), -60.0f);
        busViews[0].applyGain (gainLin);

        // The raw preview player (slice scrubbing), on Main after the master:
        // it is a monitoring signal, not part of the mix.
        if (previewActive.load (std::memory_order_acquire))
        {
            if (auto snippet = previewSampleBuffer.load())
            {
                const int gen = previewGeneration.load (std::memory_order_acquire);
                if (gen != previewPlayedGeneration)
                {
                    previewPlayedGeneration = gen;
                    previewPosition = 0.0;
                }

                const int    len  = snippet->getNumSamples();
                const int    chs  = snippet->getNumChannels();
                const double step = snippet->getSourceSampleRate() / hostRate;
                auto& mainBus = busViews[0];

                for (int i = 0; i < numSamples; ++i)
                {
                    const int idx = static_cast<int> (previewPosition);
                    if (idx + 1 >= len)
                    {
                        previewActive.store (false, std::memory_order_release);
                        break;
                    }
                    const float frac = static_cast<float> (previewPosition - idx);
                    for (int ch = 0; ch < juce::jmin (2, mainBus.getNumChannels()); ++ch)
                    {
                        const auto* src = snippet->getReadPointer (juce::jmin (ch, chs - 1));
                        mainBus.addSample (ch, i, src[idx] + frac * (src[idx + 1] - src[idx]));
                    }
                    previewPosition += step;
                }
            }
        }

        // MIDI out: only what the computer keyboard played. The host's own
        // MIDI was the input; echoing it would double every note downstream.
        midi.swapWith (liveNotes);
    }

    juce::VST3ClientExtensions* ASamplerProcessor::getVST3ClientExtensions()
    {
        return vst3Extensions.get();
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

        setLayerAudio (padIndex, layerIndex, sample, file.getFullPathName());
        maybeTrimOnLoad (padIndex, layerIndex);

        setStatus ("Loaded " + file.getFileName()
                   + " into Pad " + juce::String (padIndex + 1)
                   + " (Layer " + juce::String (layerIndex + 1) + ")");
        return LoadResult::Success;
    }

    void ASamplerProcessor::setLayerAudio (int padIndex, int layerIndex,
                                           std::shared_ptr<const Sample> sample,
                                           const juce::String& filePath, bool missing)
    {
        auto& slot = pads[(std::size_t) padIndex].layers[(std::size_t) layerIndex];
        slot.sample.store (sample != nullptr ? std::move (sample) : defaultSample);
        slot.filePath = filePath;
        slot.missing  = missing;

        // New audio on this layer: drop any stretched/formant render of the
        // previous sample and reset the render keys so the timer re-renders.
        slot.processed.store (nullptr);
        slot.renderedTimeRatio = 1.0;
        slot.renderedFormant   = 0.0;
        ++slot.renderGeneration;
        slot.formantTable.store (nullptr);
        slot.tableKey = {};
        ++slot.tableGeneration;

        // Real audio arriving ends any try-before-you-keep on the pad.
        previewOverride[(std::size_t) padIndex].store (nullptr);
        previewFilePath[(std::size_t) padIndex].clear();

        if (filePath.isNotEmpty() && ! missing)
            lastSampleFolder = juce::File (filePath).getParentDirectory();
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

        // Announce both ends: a screen-reader user needs to know a long load
        // is in flight, not just that it finished.
        setStatus ("Loading " + file.getFileName()
                   + " into Pad " + juce::String (padIndex + 1) + "...");
        startAsyncLoad (padIndex, layerIndex, file, nullptr);
    }

    void ASamplerProcessor::startAsyncLoad (int padIndex, int layerIndex, const juce::File& file,
                                            std::function<void (LoadResult)> onDone)
    {
        std::weak_ptr<bool> weakAlive = alive;

        loaderPool.addJob ([this, padIndex, layerIndex, file, weakAlive, onDone]
        {
            std::shared_ptr<const Sample> sample;
            const auto result = readSampleFile (file, sample);

            // Hop back to the message thread to mutate pad state and announce,
            // so filePath stays single-threaded as the rest of the code assumes.
            juce::MessageManager::callAsync (
                [this, padIndex, layerIndex, file, result, sample, weakAlive, onDone]
                {
                    const auto stillAlive = weakAlive.lock();
                    if (stillAlive == nullptr || ! *stillAlive)
                        return;

                    if (result == LoadResult::Success)
                    {
                        setLayerAudio (padIndex, layerIndex, sample, file.getFullPathName());
                        maybeTrimOnLoad (padIndex, layerIndex);
                        setStatus ("Loaded " + file.getFileName()
                                   + " into Pad " + juce::String (padIndex + 1)
                                   + " (Layer " + juce::String (layerIndex + 1) + ")");
                    }
                    else
                    {
                        setStatus ("Could not load " + file.getFileName()
                                   + " into Pad " + juce::String (padIndex + 1));
                    }

                    if (onDone)
                        onDone (result);
                });
        });
    }

    juce::Array<juce::File> ASamplerProcessor::listAudioFilesIn (const juce::File& folder) const
    {
        juce::Array<juce::File> files;
        if (! folder.isDirectory())
            return files;

        // Only what the format manager can actually read, so browsing never
        // lands on a file that then fails to load.
        const auto patterns = formatManager.getWildcardForAllFormats();
        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, patterns,
                                                                juce::File::findFiles))
            files.add (entry.getFile());

        struct NaturalOrder
        {
            static int compareElements (const juce::File& a, const juce::File& b)
            {
                return a.getFileName().compareNatural (b.getFileName());
            }
        };
        NaturalOrder order;
        files.sort (order);
        return files;
    }

    ASamplerProcessor::BrowseResult
    ASamplerProcessor::browseSample (int padIndex, int direction, bool synchronous)
    {
        if (padIndex < 0 || padIndex >= kNumPads || direction == 0)
            return BrowseResult::InvalidPad;

        // Browse from the edit layer's file; if that layer is empty, from any
        // loaded layer on the pad, so "next" still means something.
        const int layer = getCurrentEditLayer();
        juce::String currentPath = pads[padIndex].layers[(std::size_t) layer].filePath;
        if (currentPath.isEmpty())
            for (const auto& l : pads[padIndex].layers)
                if (l.filePath.isNotEmpty()) { currentPath = l.filePath; break; }

        if (currentPath.isEmpty())
        {
            setStatus ("Pad " + juce::String (padIndex + 1)
                       + " has no sample to browse from. Load one first.");
            return BrowseResult::NoCurrentFile;
        }

        const juce::File current (currentPath);
        const auto files = listAudioFilesIn (current.getParentDirectory());
        const int total = files.size();

        if (total == 0 || (total == 1 && files[0] == current))
        {
            setStatus ("No other samples in " + current.getParentDirectory().getFileName());
            return BrowseResult::NoOtherFiles;
        }

        // Step from the current file's position. If it was moved or deleted,
        // step from where it would sort instead, so browsing still works.
        int target = 0;
        const int index = files.indexOf (current);
        if (index >= 0)
        {
            target = ((index + (direction > 0 ? 1 : -1)) % total + total) % total;
        }
        else
        {
            int insertAt = 0;   // first file that sorts after the missing one
            while (insertAt < total
                   && files[insertAt].getFileName().compareNatural (current.getFileName()) < 0)
                ++insertAt;
            target = direction > 0 ? insertAt % total : (insertAt - 1 + total) % total;
        }

        const auto next = files[target];

        if (synchronous)
        {
            if (loadSampleForPadLayer (padIndex, layer, next) != LoadResult::Success)
                return BrowseResult::LoadFailed;
            finishBrowse (padIndex, next, target + 1, total);
            return BrowseResult::Success;
        }

        startAsyncLoad (padIndex, layer, next, [this, padIndex, next, target, total] (LoadResult r)
        {
            if (r == LoadResult::Success)
                finishBrowse (padIndex, next, target + 1, total);
        });
        return BrowseResult::Started;
    }

    void ASamplerProcessor::finishBrowse (int padIndex, const juce::File& file, int position, int total)
    {
        // Name first, since it is what the user is listening for; position
        // second, so they know where they are in the folder and when it wrapped.
        auditionPad (padIndex, 0.8f, false);
        setStatus (file.getFileNameWithoutExtension() + ", " + juce::String (position)
                   + " of " + juce::String (total));
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

        setLayerAudio (padIndex, layerIndex, nullptr, {});
        setStatus ("Reset Pad " + juce::String (padIndex + 1)
                   + " (Layer " + juce::String (layerIndex + 1) + ") to default");
    }

    int ASamplerProcessor::getCurrentChopCount() const
    {
        if (chopCountParam == nullptr) return chopCountFromIndex (kDefaultChopIndex);
        return chopCountFromIndex (juce::roundToInt (chopCountParam->load()));
    }

    int ASamplerProcessor::getCurrentEditLayer() const
    {
        if (editLayerParam == nullptr) return 0;
        return juce::jlimit (0, kNumLayers - 1,
                             juce::roundToInt (editLayerParam->load()));
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
            setLayerAudio (i, kChopLayer, sourceSample, sourceFilePath);

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

    juce::File ASamplerProcessor::getPadSampleFile (int padIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return {};
        const auto& layers = pads[(std::size_t) padIndex].layers;
        const auto& edit = layers[(std::size_t) getCurrentEditLayer()].filePath;
        if (edit.isNotEmpty())
            return juce::File (edit);
        for (const auto& l : layers)
            if (l.filePath.isNotEmpty())
                return juce::File (l.filePath);
        return {};
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

    double ASamplerProcessor::getPadReferenceSeconds (int padIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return 0.0;

        const auto sample = referenceSample (padIndex);
        if (sample == nullptr || sample->isEmpty() || sample->getSourceSampleRate() <= 0.0)
            return 0.0;
        return sample->getNumSamples() / sample->getSourceSampleRate();
    }

    juce::String ASamplerProcessor::describePad (int padIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return {};
        const auto& pad = pads[(std::size_t) padIndex];
        const auto& pp  = padParams[(std::size_t) padIndex];

        int layer = getCurrentEditLayer();
        if (pad.layers[(std::size_t) layer].filePath.isEmpty())
            for (int L = 0; L < kNumLayers; ++L)
                if (pad.layers[(std::size_t) L].filePath.isNotEmpty()) { layer = L; break; }

        int loadedLayers = 0;
        for (const auto& l : pad.layers)
            if (l.filePath.isNotEmpty()) ++loadedLayers;

        juce::StringArray parts;
        auto heading = "Pad " + juce::String (padIndex + 1) + ", "
                     + getPadLayerSampleDisplayName (padIndex, layer);
        if (loadedLayers > 1)
            heading << ", layer " << (layer + 1) << " of " << loadedLayers << " loaded";
        parts.add (heading);

        const auto& slot = pad.layers[(std::size_t) layer];
        const auto sample = slot.sample.load();
        if (slot.missing || sample == nullptr || sample->isEmpty())
        {
            parts.add (slot.missing ? "The file is missing. Use Relink samples to find it."
                                    : "No audio.");
            const auto text = parts.joinIntoString (". ") + ".";
            setStatus (text);
            return text;
        }

        const double sr    = sample->getSourceSampleRate();
        const int    total = sample->getNumSamples();
        const double fullSeconds = total / sr;

        const float startNorm = juce::jmin (pp.start->load(), pp.end->load());
        const float endNorm   = juce::jmax (pp.start->load(), pp.end->load());
        const int   s0 = juce::jlimit (0, total, juce::roundToInt (startNorm * (float) total));
        const int   e0 = juce::jlimit (s0, total, juce::roundToInt (endNorm * (float) total));

        const auto a = analyseSample (sample->getBuffer(), sr, s0, e0);

        // Length first, as played; the full file length only when trimmed.
        const bool trimmed = s0 > 0 || e0 < total;
        juce::String length = trimmed
            ? "Plays " + formatDuration (s0 / sr) + " to " + formatDuration (e0 / sr)
                + " of " + formatDuration (fullSeconds)
            : "Length " + formatDuration (fullSeconds);

        const double hostBpm = getHostBpm();
        const double bars    = a.lengthSeconds / (240.0 / hostBpm);
        length << ", " << juce::String (bars, 2) << " bars at "
               << juce::String (hostBpm, hostBpm == std::floor (hostBpm) ? 0 : 1) << " BPM";
        parts.add (length);

        parts.add ((sample->getNumChannels() > 1 ? juce::String ("Stereo, ") : juce::String ("Mono, "))
                   + juce::String (sr / 1000.0, sr == std::floor (sr / 1000.0) * 1000.0 ? 0 : 1) + " kHz");

        parts.add (a.peakDb <= -99.0f ? juce::String ("Silent")
                                      : "Peak " + juce::String (a.peakDb, 1) + " dB");

        if (a.peakDb > -99.0f)
            parts.add (juce::String (a.transientCount) + (a.transientCount == 1 ? " hit" : " hits"));

        if (a.bpm)
            parts.add ("Tempo about " + juce::String (*a.bpm, *a.bpm == std::floor (*a.bpm) ? 0 : 1) + " BPM");
        if (a.keyTonic)
            parts.add ("Key probably " + keyName (*a.keyTonic, a.keyIsMinor));
        if (a.pitchMidi)
            parts.add ("Pitch " + describePitch (*a.pitchMidi));

        const auto loop = static_cast<LoopMode> (juce::roundToInt (pp.loopMode->load()));
        if (loop != LoopMode::Off)
            parts.add (juce::String (loop == LoopMode::PingPong ? "Ping-pong loop " : "Loops ")
                       + formatDuration (pp.loopStart->load() * fullSeconds) + " to "
                       + formatDuration (pp.loopEnd->load() * fullSeconds));

        parts.add ("Plays on " + describePadTrigger (padIndex));

        const auto text = parts.joinIntoString (". ") + ".";
        setStatus (text);
        return text;
    }

    ASamplerProcessor::BounceResult
    ASamplerProcessor::bouncePad (int padIndex, const juce::File& outputDir)
    {
        BounceResult result;
        if (padIndex < 0 || padIndex >= kNumPads)
            return result;

        const double sr = getSampleRate() > 0.0 ? getSampleRate() : 44100.0;
        const int rootNote = juce::roundToInt (padParams[(std::size_t) padIndex].note->load());

        // Full velocity: a bounce is a committed version of the sound, so it
        // takes the loudest layer and the full velocity modulation.
        const int layer = selectLayerForTrigger (padIndex, rootNote, 1.0f);
        auto plan = planTrigger (padIndex, rootNote, 1.0f, layer, false);
        if (plan.sample == nullptr || plan.sample->isEmpty())
        {
            setStatus ("Pad " + juce::String (padIndex + 1) + " has nothing to bounce");
            return result;
        }
        plan.cfg.padGate = 1.0f;   // render even if the pad is muted

        Voice voice;
        voice.prepare (sr);
        voice.start (plan.sample, plan.triggerNote, plan.triggerNote, plan.cfg);

        // Loops and held notes would play forever: hold a looping pad for a
        // few cycles' worth (at least 2 s), then release so the tail renders.
        const auto& pp      = padParams[(std::size_t) padIndex];
        const bool  oneShot = pp.oneShot->load() >= 0.5f;
        const bool  looping = static_cast<LoopMode> (juce::roundToInt (pp.loopMode->load())) != LoopMode::Off;
        const double sourceSeconds = plan.sample->getNumSamples() / plan.sample->getSourceSampleRate();
        const double window = std::abs (pp.end->load() - pp.start->load()) * sourceSeconds;
        const int holdSamples = (looping && ! oneShot)
                                  ? juce::roundToInt (juce::jmax (2.0, window) * sr)
                                  : std::numeric_limits<int>::max();

        constexpr double kMaxSeconds = 60.0;
        constexpr int    kBlock      = 512;
        const int maxSamples = juce::roundToInt (kMaxSeconds * sr);

        juce::AudioBuffer<float> out (2, maxSamples);
        out.clear();

        int rendered = 0;
        bool released = false;
        while (voice.isActive() && rendered < maxSamples)
        {
            if (! released && rendered >= holdSamples)
            {
                voice.release();
                released = true;
            }
            const int n = juce::jmin (kBlock, maxSamples - rendered);
            voice.renderAdding (out, rendered, n);
            rendered += n;
        }

        // Trim the inaudible tail the release leaves behind.
        int length = rendered;
        const float floor = juce::Decibels::decibelsToGain (-90.0f);
        while (length > 0 && std::abs (out.getSample (0, length - 1)) < floor
                           && std::abs (out.getSample (1, length - 1)) < floor)
            --length;

        if (length == 0)
        {
            setStatus ("Pad " + juce::String (padIndex + 1) + " bounced to silence; nothing saved");
            return result;
        }

        const auto dir = outputDir != juce::File()
                           ? outputDir
                           : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                 .getChildFile ("ASampler").getChildFile ("bounces");
        dir.createDirectory();

        const auto sourceName = juce::File (pads[(std::size_t) padIndex].layers[(std::size_t) layer].filePath)
                                    .getFileNameWithoutExtension();
        const auto stem = "Pad " + juce::String (padIndex + 1)
                        + (sourceName.isNotEmpty() ? " " + sourceName : juce::String())
                        + " bounce";
        const auto file = dir.getNonexistentChildFile (juce::File::createLegalFileName (stem), ".wav", false);

        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (new juce::FileOutputStream (file), sr, 2, 24, {}, 0));
            if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (out, 0, length))
            {
                setStatus ("Could not write the bounce to " + dir.getFullPathName());
                return result;
            }
        }
        result.file = file;

        // Next empty pad after this one, wrapping: the usual place to put a
        // resampled sound, and never the source pad itself.
        for (int k = 1; k < kNumPads; ++k)
        {
            const int candidate = (padIndex + k) % kNumPads;
            bool empty = true;
            for (const auto& l : pads[(std::size_t) candidate].layers)
                if (l.filePath.isNotEmpty()) { empty = false; break; }
            if (empty) { result.targetPad = candidate; break; }
        }

        const auto seconds = juce::String (length / sr, 2) + " s";
        if (result.targetPad < 0)
        {
            result.ok = true;
            setStatus ("Bounced Pad " + juce::String (padIndex + 1) + ", " + seconds
                       + ". No empty pad to load it onto; saved as " + file.getFileName());
            return result;
        }

        const int target = result.targetPad;
        if (loadSampleForPadLayer (target, 0, file) != LoadResult::Success)
        {
            setStatus ("Bounced Pad " + juce::String (padIndex + 1) + " but could not load the result");
            return result;
        }

        // The bounce already carries the source pad's processing; play it
        // whole on the target so the window is not applied twice.
        beginUndoTransaction ("Bounce Pad " + juce::String (padIndex + 1));
        setRangedParam (param_ids::padStart (target + 1), 0.0f);
        setRangedParam (param_ids::padEnd   (target + 1), 1.0f);
        setRangedParam (param_ids::padLoopMode (target + 1), static_cast<float> (static_cast<int> (LoopMode::Off)));

        result.ok = true;
        setStatus ("Bounced Pad " + juce::String (padIndex + 1) + " to Pad " + juce::String (target + 1)
                   + ", " + seconds);
        return result;
    }

    juce::String ASamplerProcessor::describePadTrigger (int padIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return {};
        const auto& pp = padParams[(std::size_t) padIndex];

        const int root = juce::roundToInt (pp.note->load());
        const auto mode = static_cast<PadMode> (juce::roundToInt (pp.mode->load()));
        if (mode != PadMode::Keyboard)
            return "note " + formatNote (root);

        const int lo = juce::roundToInt (pp.lowNote->load());
        const int hi = juce::roundToInt (pp.highNote->load());
        return "keys " + formatNote (juce::jmin (lo, hi)) + " to " + formatNote (juce::jmax (lo, hi))
             + ", root " + formatNote (root);
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
        // Slice-point edits are only noted here (this can run on the audio
        // thread); the timer decides later whether to audition.
        if (parameterIndex >= 0 && parameterIndex < (int) sliceEditLookup.size())
        {
            const int code = sliceEditLookup[(std::size_t) parameterIndex];
            if (code >= 0)
            {
                const int pad = code / 2;
                sliceEditTimeMs[(std::size_t) pad].store (juce::Time::getMillisecondCounter());
                sliceEditKind[(std::size_t) pad].store ((code % 2) + 1);
                return;
            }
        }

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

            // No status announcement: the screen reader already speaks the new
            // value (host parameter list) or the newly focused pad (editor),
            // and a second voice saying the same thing is noise.
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
        return saveKitToFile (file, KitSaveMode::Paths);
    }

    bool ASamplerProcessor::saveKitToFile (const juce::File& file, KitSaveMode mode)
    {
        auto root = juce::ValueTree ("ASamplerKit");
        root.setProperty ("version", 2, nullptr);
        root.addChild (state.copyState().createCopy(), -1, nullptr);

        int copied = 0, notCopied = 0;
        const auto sampleDir = file.getSiblingFile (file.getFileNameWithoutExtension() + " samples");

        if (mode == KitSaveMode::Template)
        {
            // Settings only. Loading it leaves the samples (and names) alone,
            // like a factory kit.
            root.setProperty ("template", true, nullptr);
        }
        else
        {
            if (mode == KitSaveMode::WithSamples && ! sampleDir.createDirectory())
            {
                setStatus ("Could not create the folder " + sampleDir.getFileName());
                return false;
            }

            // Several layers often share one file (a chop puts the same file
            // on many pads), so each file is copied once.
            std::map<juce::String, juce::File> copiedAs;

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

                    if (mode == KitSaveMode::WithSamples)
                    {
                        const juce::File source (path);
                        auto found = copiedAs.find (path);
                        if (found == copiedAs.end())
                        {
                            juce::File dest = sampleDir.getChildFile (source.getFileName());
                            if (dest != source && dest.existsAsFile())
                                dest = sampleDir.getNonexistentChildFile (source.getFileNameWithoutExtension(),
                                                                          source.getFileExtension(), false);
                            const bool ok = source.existsAsFile()
                                         && (dest == source || source.copyFileTo (dest));
                            if (ok) ++copied; else ++notCopied;
                            found = copiedAs.emplace (path, ok ? dest : juce::File()).first;
                        }

                        // Stored both ways: the absolute path for this machine,
                        // and the path relative to the kit, which is what lets
                        // the kit folder move or travel to another computer.
                        if (found->second != juce::File())
                        {
                            entry.setProperty ("path", found->second.getFullPathName(), nullptr);
                            entry.setProperty ("relative", sampleDir.getFileName() + "/"
                                                           + found->second.getFileName(), nullptr);
                        }
                    }
                    samples.appendChild (entry, nullptr);
                }
            }
            root.addChild (samples, -1, nullptr);
            root.addChild (padNamesToTree(), -1, nullptr);
        }

        auto xml = root.createXml();
        if (xml == nullptr || ! xml->writeTo (file))
        {
            setStatus ("Could not save kit to " + file.getFileName());
            return false;
        }

        const auto name = file.getFileNameWithoutExtension();
        switch (mode)
        {
            case KitSaveMode::Template:
                setStatus ("Saved template " + name + " (settings only, no samples)");
                break;
            case KitSaveMode::WithSamples:
                setStatus ("Saved kit " + name + " with " + juce::String (copied) + " sample"
                           + (copied == 1 ? "" : "s") + " in the folder " + sampleDir.getFileName()
                           + (notCopied > 0 ? ", " + juce::String (notCopied) + " missing samples could not be copied"
                                            : juce::String()));
                break;
            case KitSaveMode::Paths:
            default:
                setStatus ("Saved kit " + name);
                break;
        }
        return true;
    }

    juce::File ASamplerProcessor::resolveKitSamplePath (const juce::File& kitFile,
                                                        const juce::ValueTree& entry) const
    {
        const juce::File absolute (entry.getProperty ("path", "").toString());
        if (absolute.existsAsFile())
            return absolute;

        // The kit moved (another folder, another computer): look beside it.
        if (const auto rel = entry.getProperty ("relative", "").toString(); rel.isNotEmpty())
            if (const auto candidate = kitFile.getParentDirectory().getChildFile (rel); candidate.existsAsFile())
                return candidate;

        const auto besideKit = kitFile.getSiblingFile (kitFile.getFileNameWithoutExtension() + " samples")
                                      .getChildFile (absolute.getFileName());
        if (besideKit.existsAsFile())
            return besideKit;

        return absolute;
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

        // A template carries settings only and keeps the user's samples.
        if (root.getProperty ("template", false))
        {
            setStatus ("Loaded template " + file.getFileNameWithoutExtension()
                       + " - settings only, samples unchanged");
            return true;
        }

        for (int i = 0; i < kNumPads; ++i)
            for (int L = 0; L < kNumLayers; ++L)
                setLayerAudio (i, L, nullptr, {});
        padNamesFromTree (root.getChildWithName ("padnames"));

        int restored = 0;
        int missing  = 0;
        auto samples = root.getChildWithName ("samples");
        for (int i = 0; i < samples.getNumChildren(); ++i)
        {
            auto entry = samples.getChild (i);
            const int  idx   = entry.getProperty ("index", -1);
            const int  layer = entry.getProperty ("layer", 0);
            const auto path  = resolveKitSamplePath (file, entry).getFullPathName();

            if (idx < 0 || idx >= kNumPads || layer < 0 || layer >= kNumLayers
                || entry.getProperty ("path", "").toString().isEmpty())
                continue;

            std::shared_ptr<const Sample> sample;
            if (readSampleFile (juce::File (path), sample) == LoadResult::Success)
            {
                setLayerAudio (idx, layer, sample, path);
                ++restored;
            }
            else
            {
                setLayerAudio (idx, layer, nullptr, path, true);
                ++missing;
            }
        }

        setStatus ("Loaded kit " + file.getFileNameWithoutExtension()
                   + " - " + juce::String (restored) + " samples"
                   + (missing > 0 ? (", " + juce::String (missing) + " missing") : juce::String()));
        return true;
    }

    int ASamplerProcessor::getNumPrograms()
    {
        return 1 + getFactoryKitNames().size();
    }

    int ASamplerProcessor::getCurrentProgram()
    {
        return currentProgram.load();
    }

    const juce::String ASamplerProcessor::getProgramName (int index)
    {
        if (index == 0)
            return "Current";
        const auto names = getFactoryKitNames();
        return juce::isPositiveAndBelow (index - 1, names.size()) ? names[index - 1] : juce::String();
    }

    void ASamplerProcessor::setCurrentProgram (int index)
    {
        if (! juce::isPositiveAndBelow (index, getNumPrograms()))
            return;

        // Re-selecting the current program (which hosts do after loading a
        // project) changes nothing; nor does choosing "Current".
        if (currentProgram.exchange (index) == index || index == 0)
            return;

        // Loading a kit touches every parameter and must happen on the message
        // thread, where all other edits happen. Hosts normally call this there;
        // if one doesn't, defer rather than racing the editor.
        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            loadFactoryKit (index - 1);
            return;
        }

        std::weak_ptr<bool> weakAlive = alive;
        juce::MessageManager::callAsync ([this, index, weakAlive]
        {
            if (auto a = weakAlive.lock(); a != nullptr && *a)
                loadFactoryKit (index - 1);
        });
    }

    juce::String ASamplerProcessor::getVersionString()
    {
        return JucePlugin_VersionString;
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

            case 3:  // 808 Set — every pad a long sub, pad 1 playable across the keys
                // Only pad 1 goes to Keyboard: sixteen Keyboard pads over one
                // range would all fire on every note.
                setRangedParam (param_ids::padMode (1),
                                static_cast<float> (static_cast<int> (PadMode::Keyboard)));
                setRangedParam (param_ids::padLowNote (1),  24.0f);
                setRangedParam (param_ids::padHighNote (1), 60.0f);
                for (int p = 1; p <= kNumPads; ++p)
                {
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

        // Keep the host's program selector in step when the kit came from the
        // editor rather than from the host.
        if (currentProgram.exchange (index + 1) != index + 1)
            updateHostDisplay (ChangeDetails().withProgramChanged (true));
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
        copiedPad.setProperty ("name", padNames[(std::size_t) padIndex], nullptr);

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

                if (path.isEmpty())
                {
                    setLayerAudio (padIndex, L, nullptr, {});
                    continue;
                }

                std::shared_ptr<const Sample> sample;
                if (readSampleFile (juce::File (path), sample) == LoadResult::Success)
                    setLayerAudio (padIndex, L, sample, path);
                else
                    setLayerAudio (padIndex, L, nullptr, path, true);
            }

            // The name describes the sound, so it travels with the samples.
            padNames[(std::size_t) padIndex] = copiedPad.getProperty ("name", "").toString();
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
                setLayerAudio (i, L, nullptr, {});

        // Names describe the samples just removed.
        for (auto& n : padNames)
            n.clear();

        setStatus ("Cleared all " + juce::String (kNumPads) + " pads - samples reset to default");
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

        // Speed: +100% halves the length, -100% doubles it, at any Stretch
        // Mode — it is a length control, and pitch is what Pitch is for.
        if (p.speed != nullptr)
            if (const float speed = p.speed->load(); std::abs (speed) >= 0.5f)
                options.timeRatio *= std::pow (2.0, -static_cast<double> (speed) / 100.0);

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

        // The completion is queued on the message thread and can run after
        // the processor is gone (a host removing the plugin mid-render), so
        // it checks `alive` before touching `this`.
        std::weak_ptr<bool> weakAlive = alive;

        // The job polls the generation so a render made stale by a newer
        // setting (a knob still moving, the tempo still changing) stops and
        // frees its thread instead of finishing work nobody will hear.
        auto* liveGeneration = &slot.renderGeneration;

        renderPool.addJob ([this, padIndex, layerIndex, generation, options, source, weakAlive,
                            liveGeneration]
        {
            const int numCh = source->getNumChannels();
            const int numS  = source->getNumSamples();

            juce::AudioBuffer<float> input (numCh, numS);
            for (int ch = 0; ch < numCh; ++ch)
                input.copyFrom (ch, 0, source->getReadPointer (ch), numS);

            auto rendered = processOffline (input, source->getSourceSampleRate(), options,
                                            [liveGeneration, generation]
                                            { return liveGeneration->load() != generation; });

            std::shared_ptr<const Sample> result;
            if (rendered.getNumSamples() > 0)
                result = std::make_shared<const Sample> (std::move (rendered),
                                                         source->getSourceSampleRate());

            juce::MessageManager::callAsync (
                [this, padIndex, layerIndex, generation, result, weakAlive]
                {
                    if (weakAlive.expired())
                        return;

                    auto& s = pads[padIndex].layers[(std::size_t) layerIndex];

                    // A newer request superseded this one while it ran; its
                    // result is stale, so throw it away rather than clobbering.
                    if (result != nullptr && s.renderGeneration == generation)
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
        if (p.formantCorrect == nullptr || p.mode == nullptr || p.keepLength == nullptr)
            return key;

        const bool correctFormants = p.formantCorrect->load() >= 0.5f;
        const bool keepLength      = p.keepLength->load() >= 0.5f;
        if (! correctFormants && ! keepLength)
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

        // The layer's own tuning, which planLayer adds on top.
        constantOffset += p.layerTranspose[(std::size_t) layerIndex]->load()
                        + 0.01 * p.layerFine[(std::size_t) layerIndex]->load();

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

        // The small slack keeps float rounding in the parameters (a Pitch of
        // 1e-7 rather than 0) from adding a whole semitone to either end.
        constexpr double slack = 1.0e-4;
        const int minOffset = static_cast<int> (
            std::floor (noteLow + constantOffset + velLow + slack));
        const int maxOffset = static_cast<int> (
            std::ceil (noteHigh + constantOffset + velHigh - slack));

        key.minOffset = juce::jlimit (-kMaxFormantOffset, kMaxFormantOffset, minOffset);
        key.maxOffset = juce::jlimit (-kMaxFormantOffset, kMaxFormantOffset, maxOffset);

        // A pad that only ever plays at its root pitch needs no table — the
        // single `processed` render already covers that one case.
        if (key.minOffset == 0 && key.maxOffset == 0)
            return key;

        key.enabled         = true;
        key.timeRatio       = stretch.timeRatio;
        key.formant         = stretch.formantSemitones;
        key.correctFormants = correctFormants;
        key.keepLength      = keepLength;

        return key;
    }

    std::shared_ptr<const FormantTable>
    ASamplerProcessor::buildFormantTable (const Sample& source,
                                          const FormantTableKey& key,
                                          const std::function<bool()>& shouldAbort) const
    {
        auto table = std::make_shared<FormantTable>();

        const int numCh = source.getNumChannels();
        const int numS  = source.getNumSamples();
        const double sr = source.getSourceSampleRate();

        juce::AudioBuffer<float> input (numCh, numS);
        for (int ch = 0; ch < numCh; ++ch)
            input.copyFrom (ch, 0, source.getReadPointer (ch), numS);

        std::atomic<bool> aborted { false };

        auto renderVariant = [&] (int offset)
        {
            const int slot = offset + kMaxFormantOffset;
            if (slot < 0 || slot >= kFormantVariantSlots || aborted.load())
                return;

            StretchOptions options;
            options.timeRatio = key.timeRatio;

            // Playing this variant at ratio r = 2^(offset/12) shortens it by r
            // and multiplies its formants by r. Pre-stretching by r (Keep
            // Length) and pre-shifting formants *down* by `offset` (Formant
            // Correct) leave each where it started. The user's own formant
            // offset and stretch ride on top.
            if (key.keepLength)
                options.timeRatio *= std::pow (2.0, offset / 12.0);
            options.formantSemitones = key.formant - (key.correctFormants ? offset : 0);

            if (isNeutral (options))
            {
                // Offset 0 with no user shift needs no processing at all.
                table->variants[(std::size_t) slot] =
                    std::make_shared<const Sample> (juce::AudioBuffer<float> (input), sr);
                return;
            }

            auto rendered = processOffline (input, sr, options, [&]
            {
                if (shouldAbort && shouldAbort())
                    aborted.store (true);
                return aborted.load();
            });

            if (rendered.getNumSamples() > 0)
                table->variants[(std::size_t) slot] =
                    std::make_shared<const Sample> (std::move (rendered), sr);
        };

        // Variants are independent, so they render side by side: a 4-octave
        // table is dozens of renders and would otherwise queue up on one core.
        // Each worker takes the next offset; each writes only its own slot.
        const int variants = key.maxOffset - key.minOffset + 1;
        const int workers  = juce::jlimit (1, renderThreadCount(), variants);
        std::atomic<int> next { key.minOffset };

        auto work = [&]
        {
            for (int offset = next.fetch_add (1); offset <= key.maxOffset; offset = next.fetch_add (1))
                renderVariant (offset);
        };

        std::vector<std::thread> helpers;
        for (int t = 1; t < workers; ++t)
            helpers.emplace_back (work);
        work();
        for (auto& t : helpers)
            t.join();

        return aborted.load() ? nullptr : table;
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

        // Keep Length makes the low variants longer than the source (two
        // octaves down is four times the length), so count rendered seconds.
        double renderedSeconds = 0.0;
        for (int k = key.minOffset; k <= key.maxOffset; ++k)
            renderedSeconds += seconds * key.timeRatio * (key.keepLength ? std::pow (2.0, k / 12.0) : 1.0);

        if (renderedSeconds > 120.0)
        {
            setStatus ("Per-note rendering needs "
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
                   + " per-note versions for Pad " + juce::String (padIndex + 1) + "...");

        std::weak_ptr<bool> weakAlive = alive;   // see requestRender

        auto* liveGeneration = &slot.tableGeneration;

        renderPool.addJob ([this, padIndex, layerIndex, generation, key, source, weakAlive,
                            liveGeneration]
        {
            auto table = buildFormantTable (*source, key, [liveGeneration, generation]
                                            { return liveGeneration->load() != generation; });

            juce::MessageManager::callAsync (
                [this, padIndex, layerIndex, generation, table, weakAlive]
                {
                    if (weakAlive.expired())
                        return;

                    auto& s = pads[padIndex].layers[(std::size_t) layerIndex];

                    if (table != nullptr && s.tableGeneration == generation)
                        s.formantTable.store (table);

                    if (pendingRenders.fetch_sub (1, std::memory_order_acq_rel) == 1)
                        setStatus ("Per-note rendering ready");
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
        processEditAuditions();
        processPendingNoteLearn();

        // Slice-point parameters display as time in the pad's sample, so a
        // sample swap changes their text without changing their value. Hosts
        // cache that text; tell them to re-read it. Checked here rather than at
        // every swap site (load, reset, paste, kit, relink...) so none is missed.
        {
            bool lengthsChanged = false;
            for (int p = 0; p < kNumPads; ++p)
            {
                const double secs = getPadReferenceSeconds (p);
                if (std::abs (secs - lastReportedPadSeconds[(std::size_t) p]) > 1.0e-9)
                {
                    lastReportedPadSeconds[(std::size_t) p] = secs;
                    lengthsChanged = true;
                }
            }
            if (lengthsChanged)
                updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
        }

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
                    if (++slot.pendingTableTicks >= 3)   // ~300 ms of stillness
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

    void ASamplerProcessor::auditionPad (int padIndex, float velocity, bool announce)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return;

        // Hold for the length of the played window, capped so a long loop
        // doesn't play for a minute.
        const auto& pp = padParams[(std::size_t) padIndex];
        const double window = std::abs (pp.end->load() - pp.start->load())
                            * getPadReferenceSeconds (padIndex);
        queueAudition (padIndex, velocity, -1.0f, juce::jlimit (0.05, 4.0, window));

        if (announce)
            setStatus ("Audition Pad " + juce::String (padIndex + 1)
                       + ": " + getPadSampleDisplayName (padIndex));
    }

    void ASamplerProcessor::queueAudition (int padIndex, float velocity, float startNorm,
                                           double holdSeconds)
    {
        // Hand off to the audio thread rather than triggering here: voices are
        // owned by processBlock and must not be started from the message thread.
        // The companions are stored first; the release store publishes them.
        pendingAuditionStart[(std::size_t) padIndex].store (startNorm, std::memory_order_relaxed);
        pendingAuditionHoldMs[(std::size_t) padIndex].store (
            juce::roundToInt (holdSeconds * 1000.0), std::memory_order_relaxed);

        const int encoded = juce::jlimit (1, 1000,
            juce::roundToInt (juce::jlimit (0.0f, 1.0f, velocity) * 1000.0f));
        pendingAudition[(std::size_t) padIndex].store (encoded, std::memory_order_release);
    }

    void ASamplerProcessor::auditionPadFrom (int padIndex, float startNorm, double seconds)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return;
        queueAudition (padIndex, 0.8f, juce::jlimit (0.0f, 1.0f, startNorm),
                       juce::jlimit (0.05, 4.0, seconds));
    }

    void ASamplerProcessor::processEditAuditions()
    {
        constexpr juce::uint32 settleMs = 200;
        const auto now = juce::Time::getMillisecondCounter();

        const bool enabled = auditionOnEditParam != nullptr && auditionOnEditParam->load() >= 0.5f
                          && ! transportPlaying.load (std::memory_order_relaxed);

        int pending = 0, settled = 0, padToPlay = -1, kind = 0;
        for (int p = 0; p < kNumPads; ++p)
        {
            const int k = sliceEditKind[(std::size_t) p].load();
            if (k == 0) continue;
            ++pending;
            if (now - sliceEditTimeMs[(std::size_t) p].load() >= settleMs)
            {
                ++settled;
                padToPlay = p;
                kind = k;
            }
        }

        if (pending == 0 || settled < pending)
            return;   // nothing to do, or still moving: wait for it to settle

        for (auto& k : sliceEditKind)
            k.store (0);

        // Several pads changed together: a chop, kit load, paste-all or undo
        // of one, not someone placing a slice point. Stay quiet.
        if (! enabled || pending > 1 || padToPlay < 0)
            return;

        const auto& pp   = padParams[(std::size_t) padToPlay];
        const float start = pp.start->load();
        const float end   = pp.end->load();

        if (kind == 1)
        {
            // Start moved: play from it, as a hit would.
            const double window = std::abs (end - start) * getPadReferenceSeconds (padToPlay);
            auditionPadFrom (padToPlay, start, juce::jlimit (0.05, 2.0, window));
        }
        else
        {
            // End moved: what matters is what happens just before the cut,
            // so play the last ~300 ms leading up to it.
            const double length = getPadReferenceSeconds (padToPlay);
            const float  lead   = length > 0.0 ? static_cast<float> (0.3 / length) : 0.1f;
            const float  from   = juce::jlimit (0.0f, 1.0f, juce::jmax (juce::jmin (start, end), end - lead));
            auditionPadFrom (padToPlay, from, 0.35);
        }
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

        root.addChild (padNamesToTree(), -1, nullptr);

        root.setProperty ("currentProgram", currentProgram.load(), nullptr);

        if (auto xml = root.createXml())
            copyXmlToBinary (*xml, dest);
    }

    void ASamplerProcessor::setStateInformation (const void* data, int size)
    {
        auto xml = getXmlFromBinary (data, size);
        if (xml == nullptr) return;

        auto root = juce::ValueTree::fromXml (*xml);

        // Something that isn't our state (another plugin's chunk, a kit file,
        // a damaged project): keep the current settings rather than replacing
        // them with nonsense, and say so.
        if (! root.isValid() || ! root.hasType (state.state.getType()))
        {
            setStatus ("Could not read the saved ASampler state; keeping current settings");
            return;
        }

        // A damaged project can carry non-finite values. Passed through, a NaN
        // reaches the audio thread and silences (or blows up) the output, with
        // nothing on screen to explain it. Drop those entries so the
        // parameters fall back to their defaults.
        for (int i = root.getNumChildren(); --i >= 0;)
        {
            auto child = root.getChild (i);
            if (child.hasType ("PARAM") && child.hasProperty ("value"))
            {
                const double v = child.getProperty ("value");
                if (! std::isfinite (v))
                    root.removeChild (i, nullptr);
            }
        }

        // Pull samples and midilearn out of the tree before handing the
        // rest to the param state (replaceState would otherwise expose
        // these nodes to the parameter system).
        auto samples = root.getChildWithName ("samples");
        if (samples.isValid())
            root.removeChild (samples, nullptr);

        auto learn = root.getChildWithName ("midilearn");
        if (learn.isValid())
            root.removeChild (learn, nullptr);

        auto names = root.getChildWithName ("padnames");
        if (names.isValid())
            root.removeChild (names, nullptr);
        padNamesFromTree (names);

        // Restored, not applied: the parameters below already hold whatever
        // the kit set up plus the user's edits since.
        currentProgram.store (juce::jlimit (0, getNumPrograms() - 1,
                                            (int) root.getProperty ("currentProgram", 0)));
        root.removeProperty ("currentProgram", nullptr);

        state.replaceState (root);

        // Nodes the saved state lacked were just recreated without a "value"
        // property; give them one so their first edit is undoable. And start
        // a fresh undo history: undoing into the previous project would be
        // baffling.
        materialiseParameterValueProperties();
        undoManager.clearUndoHistory();

        for (int i = 0; i < kNumPads; ++i)
            for (int L = 0; L < kNumLayers; ++L)
                setLayerAudio (i, L, nullptr, {});

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

                if (readSampleFile (juce::File (path), sample) == LoadResult::Success)
                {
                    setLayerAudio (idx, layer, sample, path);
                    ++restored;
                }
                else
                {
                    // Keep the reference so it survives a re-save and can be
                    // repaired by relinkMissingSamples() — dropping it here
                    // would quietly lose the user's kit.
                    setLayerAudio (idx, layer, nullptr, path, true);
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

    // ---------------- Reference sample ----------------

    std::shared_ptr<const Sample> ASamplerProcessor::referenceSample (int padIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return nullptr;

        // A candidate being tried is what the slice points apply to right now.
        if (auto candidate = previewOverride[(std::size_t) padIndex].load())
            return candidate;

        // The edit layer if it holds a sample, else the first that does, else
        // layer 0 (the built-in tone). Matches what the user is editing.
        const auto& layers = pads[(std::size_t) padIndex].layers;
        int chosen = getCurrentEditLayer();
        if (layers[(std::size_t) chosen].filePath.isEmpty())
        {
            chosen = 0;
            for (int L = 0; L < kNumLayers; ++L)
                if (layers[(std::size_t) L].filePath.isNotEmpty()) { chosen = L; break; }
        }
        return layers[(std::size_t) chosen].sample.load();
    }

    // ---------------- Pad names ----------------

    void ASamplerProcessor::setPadName (int padIndex, const juce::String& name)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return;

        const auto clean = name.trim().substring (0, 64);
        padNames[(std::size_t) padIndex] = clean;
        setStatus (clean.isEmpty() ? "Pad " + juce::String (padIndex + 1) + " name cleared"
                                   : "Pad " + juce::String (padIndex + 1) + " named " + clean);
    }

    juce::String ASamplerProcessor::getPadName (int padIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return {};
        return padNames[(std::size_t) padIndex];
    }

    juce::ValueTree ASamplerProcessor::padNamesToTree() const
    {
        juce::ValueTree names ("padnames");
        for (int i = 0; i < kNumPads; ++i)
        {
            if (padNames[(std::size_t) i].isEmpty()) continue;
            juce::ValueTree entry ("pad");
            entry.setProperty ("index", i, nullptr);
            entry.setProperty ("name", padNames[(std::size_t) i], nullptr);
            names.appendChild (entry, nullptr);
        }
        return names;
    }

    void ASamplerProcessor::padNamesFromTree (const juce::ValueTree& names)
    {
        for (auto& n : padNames)
            n.clear();

        if (! names.isValid()) return;
        for (int i = 0; i < names.getNumChildren(); ++i)
        {
            const auto entry = names.getChild (i);
            const int idx = entry.getProperty ("index", -1);
            if (idx >= 0 && idx < kNumPads)
                padNames[(std::size_t) idx] = entry.getProperty ("name", "").toString().substring (0, 64);
        }
    }

    // ---------------- Apply a setting to many pads ----------------

    int ASamplerProcessor::getNumPadControls()
    {
        return (int) padParamSpecs().size();
    }

    juce::String ASamplerProcessor::getPadControlName (int index)
    {
        if (index < 0 || index >= getNumPadControls()) return {};
        return padParamSpecs()[(std::size_t) index].name;
    }

    juce::String ASamplerProcessor::getPadControlSection (int index)
    {
        if (index < 0 || index >= getNumPadControls()) return {};
        return padParamSpecs()[(std::size_t) index].section;
    }

    juce::String ASamplerProcessor::getPadControlValueText (int padIndex, int controlIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads || controlIndex < 0 || controlIndex >= getNumPadControls())
            return {};
        if (auto* p = state.getParameter (padParamSpecs()[(std::size_t) controlIndex].idFor (padIndex + 1)))
            return p->getCurrentValueAsText();
        return {};
    }

    namespace
    {
        juce::String describePadRange (int firstPad, int lastPad)
        {
            return firstPad == lastPad ? "Pad " + juce::String (firstPad + 1)
                                       : "Pads " + juce::String (firstPad + 1) + " to " + juce::String (lastPad + 1);
        }
    }

    int ASamplerProcessor::applyControlToPads (int sourcePad, int controlIndex, int firstPad, int lastPad)
    {
        if (sourcePad < 0 || sourcePad >= kNumPads || controlIndex < 0 || controlIndex >= getNumPadControls())
            return 0;
        firstPad = juce::jlimit (0, kNumPads - 1, firstPad);
        lastPad  = juce::jlimit (firstPad, kNumPads - 1, lastPad);

        const auto& spec = padParamSpecs()[(std::size_t) controlIndex];
        auto* source = state.getParameter (spec.idFor (sourcePad + 1));
        if (source == nullptr) return 0;
        const float value = source->convertFrom0to1 (source->getValue());

        beginUndoTransaction ("Apply " + juce::String (spec.name) + " to pads");
        int changed = 0;
        for (int p = firstPad; p <= lastPad; ++p)
        {
            if (p == sourcePad) continue;
            auto* dest = state.getParameter (spec.idFor (p + 1));
            if (dest == nullptr || dest->getValue() == source->getValue()) continue;
            setRangedParam (spec.idFor (p + 1), value);
            ++changed;
        }

        setStatus (juce::String (spec.name) + " " + source->getCurrentValueAsText() + " from Pad "
                   + juce::String (sourcePad + 1) + " applied to " + describePadRange (firstPad, lastPad)
                   + ", " + juce::String (changed) + (changed == 1 ? " pad changed" : " pads changed"));
        return changed;
    }

    int ASamplerProcessor::applySectionToPads (int sourcePad, const juce::String& section,
                                               int firstPad, int lastPad)
    {
        if (sourcePad < 0 || sourcePad >= kNumPads) return 0;
        firstPad = juce::jlimit (0, kNumPads - 1, firstPad);
        lastPad  = juce::jlimit (firstPad, kNumPads - 1, lastPad);

        beginUndoTransaction ("Apply " + section + " to pads");
        int changed = 0;
        for (int p = firstPad; p <= lastPad; ++p)
        {
            if (p == sourcePad) continue;
            bool padChanged = false;
            for (const auto& spec : padParamSpecs())
            {
                if (section != spec.section) continue;
                auto* source = state.getParameter (spec.idFor (sourcePad + 1));
                auto* dest   = state.getParameter (spec.idFor (p + 1));
                if (source == nullptr || dest == nullptr || dest->getValue() == source->getValue()) continue;
                setRangedParam (spec.idFor (p + 1), source->convertFrom0to1 (source->getValue()));
                padChanged = true;
            }
            if (padChanged) ++changed;
        }

        setStatus (section + " settings from Pad " + juce::String (sourcePad + 1) + " applied to "
                   + describePadRange (firstPad, lastPad) + ", " + juce::String (changed)
                   + (changed == 1 ? " pad changed" : " pads changed"));
        return changed;
    }

    // ---------------- Note learn ----------------

    namespace
    {
        const char* noteLearnTargetName (int target)
        {
            switch (target)
            {
                case 1:  return "Low Note";
                case 2:  return "High Note";
                default: return "Note";
            }
        }
    }

    void ASamplerProcessor::armNoteLearn (int padIndex, NoteLearnTarget target)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return;

        learnedNote.store (-1, std::memory_order_release);
        noteLearnTarget.store ((int) target, std::memory_order_release);
        noteLearnPad.store (padIndex, std::memory_order_release);
        setStatus ("Play a note to set Pad " + juce::String (padIndex + 1) + " "
                   + noteLearnTargetName ((int) target) + ". Escape cancels.");
    }

    void ASamplerProcessor::cancelNoteLearn()
    {
        if (noteLearnPad.exchange (-1) < 0) return;
        learnedNote.store (-1, std::memory_order_release);
        setStatus ("Note learn cancelled");
    }

    void ASamplerProcessor::processPendingNoteLearn()
    {
        const int pad = noteLearnPad.load (std::memory_order_acquire);
        if (pad < 0) return;
        const int note = learnedNote.load (std::memory_order_acquire);
        if (note < 0) return;

        // Disarm first, so the audio thread cannot capture a second note.
        noteLearnPad.store (-1, std::memory_order_release);
        learnedNote.store (-1, std::memory_order_release);

        const int target = noteLearnTarget.load();
        const int p = pad + 1;
        beginUndoTransaction ("Learn note for Pad " + juce::String (p));

        const auto& pp = padParams[(std::size_t) pad];
        switch (target)
        {
            case 1:
                setRangedParam (param_ids::padLowNote (p), (float) note);
                if (juce::roundToInt (pp.highNote->load()) < note)   // keep the range the right way up
                    setRangedParam (param_ids::padHighNote (p), (float) note);
                break;
            case 2:
                setRangedParam (param_ids::padHighNote (p), (float) note);
                if (juce::roundToInt (pp.lowNote->load()) > note)
                    setRangedParam (param_ids::padLowNote (p), (float) note);
                break;
            default:
                setRangedParam (param_ids::padNote (p), (float) note);
                break;
        }

        setStatus ("Pad " + juce::String (p) + " " + noteLearnTargetName (target) + " set to " + formatNote (note));
    }

    // ---------------- Loading many files at once ----------------

    int ASamplerProcessor::loadSamples (int startPad, const juce::Array<juce::File>& filesIn,
                                        MultiLoadMode mode, bool synchronous)
    {
        if (startPad < 0 || startPad >= kNumPads) return 0;

        // Natural order, so "kick 2" lands before "kick 10" whatever order the
        // file dialog (or Explorer) handed them over in.
        juce::Array<juce::File> files;
        for (const auto& f : filesIn)
            if (f.existsAsFile()) files.add (f);
        struct NaturalOrder
        {
            static int compareElements (const juce::File& a, const juce::File& b)
            {
                return a.getFileName().compareNatural (b.getFileName());
            }
        };
        NaturalOrder order;
        files.sort (order);

        const int room = mode == MultiLoadMode::OnePerPad ? kNumPads - startPad : kNumLayers;
        const int skipped = juce::jmax (0, files.size() - room);
        files.removeRange (room, files.size());

        if (files.isEmpty())
        {
            setStatus ("No audio files to load");
            return 0;
        }

        auto readAll = [this, files]
        {
            std::vector<std::shared_ptr<const Sample>> samples ((std::size_t) files.size());
            for (int i = 0; i < files.size(); ++i)
                if (readSampleFile (files[i], samples[(std::size_t) i]) != LoadResult::Success)
                    samples[(std::size_t) i] = nullptr;
            return samples;
        };

        if (synchronous)
        {
            applyMultiLoad (startPad, mode, files, readAll());
            if (skipped > 0)
                setStatus (getLastStatusMessage() + ". " + juce::String (skipped) + " more did not fit and were skipped");
            return files.size();
        }

        setStatus ("Loading " + juce::String (files.size()) + " samples...");
        std::weak_ptr<bool> weakAlive = alive;
        loaderPool.addJob ([this, startPad, mode, files, readAll, skipped, weakAlive]
        {
            auto samples = readAll();
            juce::MessageManager::callAsync ([this, startPad, mode, files, samples, skipped, weakAlive]
            {
                const auto stillAlive = weakAlive.lock();
                if (stillAlive == nullptr || ! *stillAlive)
                    return;
                applyMultiLoad (startPad, mode, files, samples);
                if (skipped > 0)
                    setStatus (getLastStatusMessage() + ". " + juce::String (skipped) + " more did not fit and were skipped");
            });
        });
        return files.size();
    }

    void ASamplerProcessor::applyMultiLoad (int startPad, MultiLoadMode mode,
                                            const juce::Array<juce::File>& files,
                                            const std::vector<std::shared_ptr<const Sample>>& samples)
    {
        beginUndoTransaction ("Load " + juce::String (files.size()) + " samples");

        int loaded = 0, failed = 0;
        juce::String firstFailed;
        for (int i = 0; i < files.size(); ++i)
        {
            const auto& sample = samples[(std::size_t) i];
            const int pad   = mode == MultiLoadMode::OnePerPad ? startPad + i : startPad;
            const int layer = mode == MultiLoadMode::OnePerPad ? 0 : i;
            if (sample == nullptr)
            {
                ++failed;
                if (firstFailed.isEmpty()) firstFailed = files[i].getFileName();
                continue;
            }
            setLayerAudio (pad, layer, sample, files[i].getFullPathName());
            maybeTrimOnLoad (pad, layer);
            ++loaded;
        }

        juce::String what;
        if (mode == MultiLoadMode::OnePerPad)
        {
            what = "onto " + describePadRange (startPad, startPad + files.size() - 1);
        }
        else
        {
            // The pad becomes exactly these layers: leftovers from an earlier
            // sound would otherwise join the rotation or the velocity stack.
            for (int L = files.size(); L < kNumLayers; ++L)
                setLayerAudio (startPad, L, nullptr, {});
            for (int L = 0; L < files.size(); ++L)
                setRangedParam (param_ids::padLayerTrigger (startPad + 1, L + 1), 0.0f);

            const int p = startPad + 1;
            if (mode == MultiLoadMode::RoundRobin)
            {
                setRangedParam (param_ids::padLayerMode (p), (float) static_cast<int> (LayerMode::RoundRobin));
                what = "as round-robin layers on Pad " + juce::String (p);
            }
            else
            {
                setRangedParam (param_ids::padLayerMode (p), (float) static_cast<int> (LayerMode::Velocity));
                spreadVelocityLayersImpl (startPad, false);
                what = "as velocity layers on Pad " + juce::String (p) + ", softest first";
            }
        }

        auto status = "Loaded " + juce::String (loaded) + (loaded == 1 ? " sample " : " samples ") + what;
        if (failed > 0)
            status << ". " << failed << " could not be read, starting with " << firstFailed;
        setStatus (status);
    }

    int ASamplerProcessor::spreadVelocityLayers (int padIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return 0;
        beginUndoTransaction ("Spread velocity layers on Pad " + juce::String (padIndex + 1));
        return spreadVelocityLayersImpl (padIndex, true);
    }

    int ASamplerProcessor::spreadVelocityLayersImpl (int padIndex, bool announce)
    {
        const auto& pp = padParams[(std::size_t) padIndex];
        std::vector<int> layers;
        for (int L = 0; L < kNumLayers; ++L)
            if (layerIsLoaded (padIndex, L) && juce::roundToInt (pp.layerTrigger[(std::size_t) L]->load()) == 0)
                layers.push_back (L);

        const int n = (int) layers.size();
        if (n == 0)
        {
            if (announce)
                setStatus ("Pad " + juce::String (padIndex + 1) + " has no loaded layers to spread");
            return 0;
        }

        juce::StringArray ranges;
        for (int i = 0; i < n; ++i)
        {
            const int lo = juce::roundToInt (i * 128.0 / n);
            const int hi = juce::roundToInt ((i + 1) * 128.0 / n) - 1;
            setRangedParam (param_ids::padLayerVelLow  (padIndex + 1, layers[(std::size_t) i] + 1), (float) lo);
            setRangedParam (param_ids::padLayerVelHigh (padIndex + 1, layers[(std::size_t) i] + 1), (float) hi);
            ranges.add ("Layer " + juce::String (layers[(std::size_t) i] + 1) + " " + juce::String (lo)
                        + " to " + juce::String (hi));
        }

        if (announce)
            setStatus ("Velocity ranges on Pad " + juce::String (padIndex + 1) + ": " + ranges.joinIntoString (", "));
        return n;
    }

    // ---------------- Silence trimming ----------------

    bool ASamplerProcessor::trimSilence (int padIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return false;
        beginUndoTransaction ("Trim silence on Pad " + juce::String (padIndex + 1));
        return trimSilenceImpl (padIndex, referenceSample (padIndex), false, true);
    }

    void ASamplerProcessor::maybeTrimOnLoad (int padIndex, int layerIndex)
    {
        if (trimOnLoadParam == nullptr || trimOnLoadParam->load() < 0.5f) return;
        // Start/End are pad-wide, so measure the layer the user is working on.
        if (layerIndex != 0 && layerIndex != getCurrentEditLayer()) return;
        trimSilenceImpl (padIndex, pads[(std::size_t) padIndex].layers[(std::size_t) layerIndex].sample.load(),
                         true, false);
    }

    bool ASamplerProcessor::trimSilenceImpl (int padIndex, std::shared_ptr<const Sample> sample,
                                             bool wholeSample, bool announce)
    {
        if (sample == nullptr || sample->isEmpty() || sample->getSourceSampleRate() <= 0.0)
            return false;

        const int    len = sample->getNumSamples();
        const double sr  = sample->getSourceSampleRate();
        const int    p   = padIndex + 1;
        const auto&  pp  = padParams[(std::size_t) padIndex];

        int winStart = 0, winEnd = len;
        if (! wholeSample)
        {
            const float a = pp.start->load(), b = pp.end->load();
            winStart = juce::jlimit (0, len - 1, (int) (juce::jmin (a, b) * len));
            winEnd   = juce::jlimit (winStart + 1, len, (int) std::ceil (juce::jmax (a, b) * len));
        }

        auto levelAt = [&] (int i)
        {
            float v = 0.0f;
            for (int ch = 0; ch < sample->getNumChannels(); ++ch)
                v = juce::jmax (v, std::abs (sample->getReadPointer (ch)[i]));
            return v;
        };

        float peak = 0.0f;
        for (int i = winStart; i < winEnd; ++i)
            peak = juce::jmax (peak, levelAt (i));

        if (peak < 1.0e-5f)
        {
            if (announce)
                setStatus ("Pad " + juce::String (p) + " is silent, nothing to trim");
            return false;
        }

        // The onset is where the sound reaches 30 dB under its peak (backed off
        // 1 ms so the attack isn't clipped); the tail ends at 60 dB under
        // (plus 5 ms so the decay fades naturally).
        const float startThreshold = peak * juce::Decibels::decibelsToGain (-30.0f);
        const float endThreshold   = peak * juce::Decibels::decibelsToGain (-60.0f);

        int first = winStart;
        while (first < winEnd && levelAt (first) < startThreshold) ++first;
        int last = winEnd - 1;
        while (last > first && levelAt (last) < endThreshold) --last;

        const int newStart = juce::jmax (winStart, first - (int) (0.001 * sr));
        const int newEnd   = juce::jmin (winEnd, last + 1 + (int) (0.005 * sr));

        const float startNorm = (float) newStart / (float) len;
        const float endNorm   = (float) newEnd / (float) len;
        setRangedParam (param_ids::padStart (p), startNorm);
        setRangedParam (param_ids::padEnd (p), endNorm);

        if (announce)
        {
            const double seconds = len / sr;
            setStatus ("Trimmed Pad " + juce::String (p) + ": Start " + formatSlicePoint (startNorm, seconds)
                       + ", End " + formatSlicePoint (endNorm, seconds));
        }
        return true;
    }

    // ---------------- Raw preview player ----------------

    void ASamplerProcessor::playPreview (std::shared_ptr<const Sample> sample)
    {
        if (sample == nullptr || sample->getNumSamples() < 2) return;

        // Keep the outgoing buffer (and the one before) alive on this thread:
        // the audio thread may still hold a snapshot of it.
        if (auto old = previewSampleBuffer.load())
            previewRetired.push_back (std::move (old));
        while (previewRetired.size() > 4)
            previewRetired.erase (previewRetired.begin());

        previewSampleBuffer.store (std::move (sample));
        previewGeneration.fetch_add (1, std::memory_order_acq_rel);
        previewActive.store (true, std::memory_order_release);
    }

    void ASamplerProcessor::stopPreview()
    {
        previewActive.store (false, std::memory_order_release);
    }

    // ---------------- Slice-point scrubbing ----------------

    void ASamplerProcessor::previewSlicePoint (int padIndex, bool isEnd, float norm)
    {
        const auto sample = referenceSample (padIndex);
        if (sample == nullptr || sample->getNumSamples() < 2 || sample->getSourceSampleRate() <= 0.0)
            return;

        const int    len = sample->getNumSamples();
        const double sr  = sample->getSourceSampleRate();
        const int    pos = juce::jlimit (0, len, juce::roundToInt (juce::jlimit (0.0f, 1.0f, norm) * len));

        // Start: the 80 ms that follow it. End: the 150 ms that lead up to it,
        // backwards, so the first thing heard is exactly what is being cut.
        const int from = isEnd ? juce::jmax (0, pos - (int) (0.150 * sr)) : pos;
        const int to   = isEnd ? pos : juce::jmin (len, pos + (int) (0.080 * sr));
        const int n    = to - from;
        if (n < 2)
            return;

        const int chs = sample->getNumChannels();
        juce::AudioBuffer<float> snippet (chs, n);
        for (int ch = 0; ch < chs; ++ch)
        {
            const auto* src = sample->getReadPointer (ch) + from;
            auto* dst = snippet.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
                dst[i] = isEnd ? src[n - 1 - i] : src[i];
        }

        const int fadeIn  = juce::jmin (n / 4, (int) (0.002 * sr));
        const int fadeOut = juce::jmin (n / 4, (int) (0.010 * sr));
        if (fadeIn > 0)  snippet.applyGainRamp (0, fadeIn, 0.0f, 1.0f);
        if (fadeOut > 0) snippet.applyGainRamp (n - fadeOut, fadeOut, 1.0f, 0.0f);

        playPreview (std::make_shared<const Sample> (std::move (snippet), sr));
    }

    void ASamplerProcessor::setSlicePoint (int padIndex, bool isEnd, float norm)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return;
        const int p = padIndex + 1;
        norm = juce::jlimit (0.0f, 1.0f, norm);

        beginUndoTransaction ((isEnd ? "Set End on Pad " : "Set Start on Pad ") + juce::String (p));
        setRangedParam (isEnd ? param_ids::padEnd (p) : param_ids::padStart (p), norm);
        setStatus ("Pad " + juce::String (p) + (isEnd ? " End " : " Start ")
                   + formatSlicePoint (norm, getPadReferenceSeconds (padIndex)));
    }

    float ASamplerProcessor::getSlicePoint (int padIndex, bool isEnd) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return 0.0f;
        const auto& pp = padParams[(std::size_t) padIndex];
        return isEnd ? pp.end->load() : pp.start->load();
    }

    // ---------------- Sample preview (try before you keep) ----------------

    ASamplerProcessor::BrowseResult
    ASamplerProcessor::previewSample (int padIndex, int direction, bool synchronous)
    {
        if (padIndex < 0 || padIndex >= kNumPads || direction == 0)
            return BrowseResult::InvalidPad;

        // Step from the candidate being tried, else from the pad's own sample,
        // else start in the folder of the last sample loaded anywhere.
        juce::File current (previewFilePath[(std::size_t) padIndex]);
        if (previewFilePath[(std::size_t) padIndex].isEmpty())
        {
            juce::String path = pads[(std::size_t) padIndex].layers[(std::size_t) getCurrentEditLayer()].filePath;
            if (path.isEmpty())
                for (const auto& l : pads[(std::size_t) padIndex].layers)
                    if (l.filePath.isNotEmpty()) { path = l.filePath; break; }
            current = path.isNotEmpty() ? juce::File (path) : juce::File();
        }

        const auto folder = current != juce::File() ? current.getParentDirectory() : lastSampleFolder;
        if (! folder.isDirectory())
        {
            setStatus ("Nothing to try yet on Pad " + juce::String (padIndex + 1)
                       + ". Load a sample from the folder you want to browse first.");
            return BrowseResult::NoCurrentFile;
        }

        const auto files = listAudioFilesIn (folder);
        const int total = files.size();
        if (total == 0 || (total == 1 && files[0] == current))
        {
            setStatus ("No other samples in " + folder.getFileName());
            return BrowseResult::NoOtherFiles;
        }

        int target;
        const int index = current != juce::File() ? files.indexOf (current) : -1;
        if (index >= 0)
            target = ((index + (direction > 0 ? 1 : -1)) % total + total) % total;
        else
            target = direction > 0 ? 0 : total - 1;

        return startPreview (padIndex, files[target], target + 1, total, synchronous);
    }

    ASamplerProcessor::BrowseResult
    ASamplerProcessor::previewSampleFile (int padIndex, const juce::File& file, bool synchronous)
    {
        if (padIndex < 0 || padIndex >= kNumPads)
            return BrowseResult::InvalidPad;
        const auto files = listAudioFilesIn (file.getParentDirectory());
        const int index = files.indexOf (file);
        return startPreview (padIndex, file, index >= 0 ? index + 1 : 1, juce::jmax (1, files.size()), synchronous);
    }

    ASamplerProcessor::BrowseResult
    ASamplerProcessor::startPreview (int padIndex, const juce::File& file, int position, int total,
                                     bool synchronous)
    {
        if (synchronous)
        {
            std::shared_ptr<const Sample> sample;
            if (readSampleFile (file, sample) != LoadResult::Success)
            {
                setStatus ("Could not read " + file.getFileName());
                return BrowseResult::LoadFailed;
            }
            applyPreview (padIndex, file, std::move (sample), position, total);
            return BrowseResult::Success;
        }

        std::weak_ptr<bool> weakAlive = alive;
        loaderPool.addJob ([this, padIndex, file, position, total, weakAlive]
        {
            std::shared_ptr<const Sample> sample;
            const auto result = readSampleFile (file, sample);
            juce::MessageManager::callAsync ([this, padIndex, file, position, total, sample, result, weakAlive]
            {
                const auto stillAlive = weakAlive.lock();
                if (stillAlive == nullptr || ! *stillAlive)
                    return;
                if (result == LoadResult::Success)
                    applyPreview (padIndex, file, sample, position, total);
                else
                    setStatus ("Could not read " + file.getFileName());
            });
        });
        return BrowseResult::Started;
    }

    void ASamplerProcessor::applyPreview (int padIndex, const juce::File& file,
                                          std::shared_ptr<const Sample> sample, int position, int total)
    {
        const bool first = previewOverride[(std::size_t) padIndex].load() == nullptr;
        previewOverride[(std::size_t) padIndex].store (std::move (sample));
        previewFilePath[(std::size_t) padIndex] = file.getFullPathName();

        // While the beat plays, the candidate is simply what the pad plays in
        // it; an extra audition on top would land off the grid.
        if (! transportPlaying.load (std::memory_order_relaxed))
            auditionPad (padIndex, 0.8f, false);

        setStatus ("Trying " + file.getFileNameWithoutExtension() + ", " + juce::String (position) + " of "
                   + juce::String (total)
                   + (first ? juce::String (". Enter keeps it, Escape goes back") : juce::String()));
    }

    bool ASamplerProcessor::keepPreview (int padIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return false;
        auto sample = previewOverride[(std::size_t) padIndex].load();
        if (sample == nullptr)
        {
            setStatus ("Nothing is being tried on Pad " + juce::String (padIndex + 1));
            return false;
        }

        const juce::File file (previewFilePath[(std::size_t) padIndex]);
        const int layer = getCurrentEditLayer();
        setLayerAudio (padIndex, layer, std::move (sample), file.getFullPathName());   // ends the preview
        maybeTrimOnLoad (padIndex, layer);
        setStatus ("Kept " + file.getFileNameWithoutExtension() + " on Pad " + juce::String (padIndex + 1));
        return true;
    }

    void ASamplerProcessor::cancelPreview (int padIndex)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return;
        if (previewOverride[(std::size_t) padIndex].load() == nullptr) return;

        previewOverride[(std::size_t) padIndex].store (nullptr);
        previewFilePath[(std::size_t) padIndex].clear();
        setStatus ("Back to " + getPadSampleDisplayName (padIndex) + " on Pad " + juce::String (padIndex + 1));
    }

    bool ASamplerProcessor::isPreviewing (int padIndex) const
    {
        return padIndex >= 0 && padIndex < kNumPads && previewOverride[(std::size_t) padIndex].load() != nullptr;
    }

    juce::File ASamplerProcessor::getPreviewFile (int padIndex) const
    {
        if (! isPreviewing (padIndex)) return {};
        return juce::File (previewFilePath[(std::size_t) padIndex]);
    }

    // ---------------- Live notes (computer keyboard) ----------------

    void ASamplerProcessor::queueLiveNote (int midiNote, bool isNoteOn)
    {
        if (midiNote < 0 || midiNote > 127) return;
        int start1, size1, start2, size2;
        liveNoteFifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 + size2 == 0) return;   // full: drop rather than block
        liveNoteData[(std::size_t) (size1 > 0 ? start1 : start2)] = midiNote | (isNoteOn ? 0x100 : 0);
        liveNoteFifo.finishedWrite (1);
    }

    int ASamplerProcessor::getPadPlayNote (int padIndex) const
    {
        if (padIndex < 0 || padIndex >= kNumPads) return -1;
        return juce::jlimit (0, 127, juce::roundToInt (padParams[(std::size_t) padIndex].note->load()));
    }

    // ---------------- REAPER ----------------

    bool ASamplerProcessor::toggleReaperRecord()
    {
        if (! reaperApi.canRecord())
        {
            setStatus ("Record works only when ASampler runs in REAPER");
            return false;
        }
        reaperApi.csurfOnRecord();
        setStatus ("Record toggled");
        return true;
    }

    bool ASamplerProcessor::grabReaperItem (int padIndex, const juce::File& outputDir)
    {
        if (padIndex < 0 || padIndex >= kNumPads) return false;
        if (! reaperApi.canGrabItems())
        {
            setStatus ("Grab item works only when ASampler runs in REAPER");
            return false;
        }

        const int selected = reaperApi.countSelectedMediaItems (nullptr);
        if (selected != 1)
        {
            setStatus (selected == 0 ? juce::String ("Select one item in REAPER first")
                                     : "Select just one item in REAPER, " + juce::String (selected) + " are selected");
            return false;
        }

        // Glue renders the item as heard (fades, rate, volume, take FX) into a
        // new file. The project is then put back with an undo; the file stays.
        const int before = reaperApi.getProjectStateChangeCount (nullptr);
        reaperApi.mainOnCommand (40257, 0);   // Item: Glue items, ignoring time selection, including fades

        juce::String rendered;
        if (auto* item = reaperApi.getSelectedMediaItem (nullptr, 0))
            if (auto* take = reaperApi.getActiveTake (item))
                if (auto* source = reaperApi.getMediaItemTakeSource (take))
                {
                    std::vector<char> buf (4096, 0);
                    reaperApi.getMediaSourceFileName (source, buf.data(), (int) buf.size() - 1);
                    rendered = juce::String::fromUTF8 (buf.data());
                }

        if (reaperApi.getProjectStateChangeCount (nullptr) != before)
            reaperApi.undoDoUndo2 (nullptr);

        const juce::File renderedFile (rendered);
        if (rendered.isEmpty() || ! renderedFile.existsAsFile())
        {
            setStatus ("REAPER did not produce a file for the selected item");
            return false;
        }

        // Copy out of the project folder: the glued file is REAPER's, and an
        // undo or a cleanup could remove it.
        auto dir = outputDir != juce::File()
                     ? outputDir
                     : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                           .getChildFile ("ASampler").getChildFile ("grabbed");
        dir.createDirectory();
        const auto dest = dir.getNonexistentChildFile ("Pad " + juce::String (padIndex + 1) + " "
                                                           + renderedFile.getFileNameWithoutExtension(),
                                                       renderedFile.getFileExtension(), false);
        if (! renderedFile.copyFileTo (dest))
        {
            setStatus ("Could not copy the grabbed item to " + dir.getFullPathName());
            return false;
        }

        if (loadSampleForPadLayer (padIndex, getCurrentEditLayer(), dest) != LoadResult::Success)
            return false;

        setStatus ("Grabbed the selected REAPER item onto Pad " + juce::String (padIndex + 1));
        return true;
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new asampler::ASamplerProcessor();
}
