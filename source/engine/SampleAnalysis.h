#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>

namespace asampler
{
    // What "Describe sample" reports: the facts a sighted producer reads off a
    // waveform at a glance (how long, how loud, how many hits), plus the
    // musical ones they would otherwise work out by ear (tempo, key, pitch).
    //
    // Every estimate is optional. A wrong tempo or key stated confidently is
    // worse than none — the user cannot glance at the waveform to sanity-check
    // it — so each detector declines when its evidence is weak.
    struct SampleAnalysis
    {
        double lengthSeconds  = 0.0;
        float  peakDb         = -100.0f;   // dBFS; -100 for silence
        float  rmsDb          = -100.0f;
        int    transientCount = 0;

        // Tempo of a loop. Only attempted for material of 2 s or more with a
        // clear periodic pulse; one-shots and slices report nothing.
        std::optional<double> bpm;

        // Key of tonal, multi-note material: tonic pitch class 0..11 (C = 0)
        // and mode. Absent for drums and for single-note samples, which have a
        // pitch rather than a key.
        std::optional<int> keyTonic;
        bool               keyIsMinor = false;

        // Fundamental of a single pitched sound (808, bass note, vocal tone)
        // as a fractional MIDI note, e.g. 29.12 = F0 (middle C = C3), 12 cents sharp.
        std::optional<float> pitchMidi;
    };

    // Analyse samples [start, end) of `audio` (all channels mixed to mono).
    // Long material is analysed over its first minute only, which bounds the
    // cost on the message thread and loses nothing a description needs.
    SampleAnalysis analyseSample (const juce::AudioBuffer<float>& audio, double sampleRate,
                                  int start = 0, int end = -1);

    // "A minor", "F# major". Pitch class 0..11.
    juce::String keyName (int tonic, bool minor);

    // "F0, 12 cents sharp" / "A3, in tune" (within 5 cents). Middle C = C3,
    // matching the note names used everywhere else in the plugin.
    juce::String describePitch (float midiNote);
}
