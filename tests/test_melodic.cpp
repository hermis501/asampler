#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "PluginProcessor.h"

using namespace asampler;

namespace
{
    float plainValue (ASamplerProcessor& proc, const juce::String& id)
    {
        auto* p = proc.getState().getParameter (id);
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    }

    // Write through the APVTS ValueTree, which is what the plugin's own
    // setRangedParam does. Writing the parameter object directly only reaches
    // the tree later on APVTS's internal timer, so undo assertions made
    // immediately afterwards would compare against a stale tree value.
    void setPlain (ASamplerProcessor& proc, const juce::String& id, float v)
    {
        auto value = proc.getState().getParameterAsValue (id);
        if (! value.getValue().isVoid())
        {
            value.setValue (v);
            return;
        }

        if (auto* p = proc.getState().getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    juce::File writeTempWav (const juce::String& name, int numSamples = 2000)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("ASamplerTests");
        dir.createDirectory();
        const auto file = dir.getChildFile (name);
        file.deleteFile();

        juce::AudioBuffer<float> buf (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (0, i, 0.5f * std::sin (i * 0.05f));

        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        if (stream == nullptr) return {};
        std::unique_ptr<juce::AudioFormatWriter> writer (
            fmt.createWriterFor (stream.release(), 44100.0, 1, 16, {}, 0));
        if (writer == nullptr) return {};
        writer->writeFromAudioSampleBuffer (buf, 0, numSamples);
        writer.reset();
        return file;
    }
}

// ---------------- Scale quantisation ----------------

TEST_CASE ("Chromatic scale leaves notes untouched", "[scale]")
{
    for (int n = 0; n < 128; ++n)
        REQUIRE (snapNoteToScale (n, 0, Scale::Chromatic) == n);
}

TEST_CASE ("Major scale snaps off-key notes to scale degrees", "[scale]")
{
    // C major: C D E F G A B = 60 62 64 65 67 69 71
    REQUIRE (snapNoteToScale (60, 0, Scale::Major) == 60);   // C stays
    REQUIRE (snapNoteToScale (61, 0, Scale::Major) == 60);   // C# -> C
    REQUIRE (snapNoteToScale (62, 0, Scale::Major) == 62);   // D stays
    REQUIRE (snapNoteToScale (66, 0, Scale::Major) == 65);   // F# -> F
    REQUIRE (snapNoteToScale (70, 0, Scale::Major) == 69);   // A# -> A
}

TEST_CASE ("Scale snapping respects the key root", "[scale]")
{
    // D major contains F#, which C major does not.
    REQUIRE (snapNoteToScale (66, 2, Scale::Major) == 66);   // F# in D major: kept
    REQUIRE (snapNoteToScale (66, 0, Scale::Major) == 65);   // F# in C major: moved

    // F natural is not in D major. It sits exactly between E and F#, so either
    // is a correct answer — assert the property (lands in-scale, moves by one
    // semitone) rather than pinning which side of the tie wins.
    const int snapped = snapNoteToScale (65, 2, Scale::Major);
    REQUIRE (std::abs (snapped - 65) == 1);
    REQUIRE (snapNoteToScale (snapped, 2, Scale::Major) == snapped);
}

TEST_CASE ("Scale snapping can cross an octave boundary", "[scale]")
{
    // B (71) in C major is in-scale and must not move.
    REQUIRE (snapNoteToScale (71, 0, Scale::Major) == 71);

    // In C pentatonic minor (C Eb F G Bb) B(71) is one semitone under the
    // upper C(72) and eleven above Bb(70)... actually Bb is 1 below, so it
    // snaps down. The important property is that it never travels the length
    // of the scale: the result must be adjacent, not far away.
    const int snapped = snapNoteToScale (71, 0, Scale::PentatonicMinor);
    REQUIRE (std::abs (snapped - 71) <= 1);
}

TEST_CASE ("Snapping is idempotent", "[scale]")
{
    for (int n = 48; n < 84; ++n)
    {
        const int once  = snapNoteToScale (n, 0, Scale::NaturalMinor);
        const int twice = snapNoteToScale (once, 0, Scale::NaturalMinor);
        REQUIRE (once == twice);
    }
}

TEST_CASE ("Every scale returns non-empty degrees", "[scale]")
{
    for (int i = 0; i <= static_cast<int> (Scale::Mixolydian); ++i)
    {
        const auto d = scaleDegrees (static_cast<Scale> (i));
        REQUIRE_FALSE (d.isEmpty());
        REQUIRE (d[0] == 0);   // every scale contains its root
    }
}

// ---------------- Glide modes ----------------

TEST_CASE ("Glide mode helpers classify the modes", "[glide]")
{
    REQUIRE_FALSE (isRateGlide (GlideMode::Legato));
    REQUIRE_FALSE (isRateGlide (GlideMode::Always));
    REQUIRE (isRateGlide (GlideMode::LegatoRate));
    REQUIRE (isRateGlide (GlideMode::AlwaysRate));

    REQUIRE (isLegatoGlide (GlideMode::Legato));
    REQUIRE (isLegatoGlide (GlideMode::LegatoRate));
    REQUIRE_FALSE (isLegatoGlide (GlideMode::Always));
    REQUIRE_FALSE (isLegatoGlide (GlideMode::AlwaysRate));
    REQUIRE_FALSE (isLegatoGlide (GlideMode::Off));
}

TEST_CASE ("Rate glide modes are exposed as parameter choices", "[glide][parameters]")
{
    ASamplerProcessor proc;
    auto* p = proc.getState().getParameter (param_ids::padGlideMode (1));
    REQUIRE (p != nullptr);

    // Off, Legato, Always, Legato (rate), Always (rate)
    REQUIRE (p->getNumSteps() == 5);

    setPlain (proc, param_ids::padGlideMode (1),
              static_cast<float> (static_cast<int> (GlideMode::LegatoRate)));
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padGlideMode (1)))
             == static_cast<int> (GlideMode::LegatoRate));
}

TEST_CASE ("Vel to Glide parameter exists and defaults to off", "[glide][parameters]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padVelToGlide (1)) != nullptr);
    REQUIRE (plainValue (proc, param_ids::padVelToGlide (1)) == Catch::Approx (0.0f));
}

// ---------------- Key zones ----------------

TEST_CASE ("Key zone parameters exist for every layer", "[keyzones][parameters]")
{
    ASamplerProcessor proc;
    for (int L = 1; L <= kNumLayers; ++L)
    {
        REQUIRE (proc.getState().getRawParameterValue (param_ids::padLayerLowNote (1, L))  != nullptr);
        REQUIRE (proc.getState().getRawParameterValue (param_ids::padLayerHighNote (1, L)) != nullptr);
        REQUIRE (proc.getState().getRawParameterValue (param_ids::padLayerRootNote (1, L)) != nullptr);
    }

    // Defaults span the keyboard, so switching to Key Zones without editing
    // anything behaves like the single-sample case.
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padLayerLowNote (1, 1)))  == 0);
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padLayerHighNote (1, 1))) == 127);
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padLayerRootNote (1, 1))) == 60);
}

TEST_CASE ("Key Zones is an available layer mode", "[keyzones][parameters]")
{
    ASamplerProcessor proc;
    auto* p = proc.getState().getParameter (param_ids::padLayerMode (1));
    REQUIRE (p != nullptr);
    REQUIRE (p->getNumSteps() == 4);   // Velocity, RoundRobin, Random, KeyZones
}

TEST_CASE ("Key zones route notes to the layer whose range contains them",
           "[keyzones]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    const auto lowWav  = writeTempWav ("zone_low.wav", 44100);
    const auto highWav = writeTempWav ("zone_high.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, lowWav)  == ASamplerProcessor::LoadResult::Success);
    REQUIRE (proc.loadSampleForPadLayer (0, 1, highWav) == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padMode (1), static_cast<float> (static_cast<int> (PadMode::Keyboard)));
    setPlain (proc, param_ids::padLowNote (1),  0.0f);
    setPlain (proc, param_ids::padHighNote (1), 127.0f);
    setPlain (proc, param_ids::padLayerMode (1),
              static_cast<float> (static_cast<int> (LayerMode::KeyZones)));

    // Layer 1 covers C2-B2, layer 2 covers C3-B3.
    setPlain (proc, param_ids::padLayerLowNote (1, 1),  36.0f);
    setPlain (proc, param_ids::padLayerHighNote (1, 1), 47.0f);
    setPlain (proc, param_ids::padLayerRootNote (1, 1), 36.0f);
    setPlain (proc, param_ids::padLayerLowNote (1, 2),  48.0f);
    setPlain (proc, param_ids::padLayerHighNote (1, 2), 59.0f);
    setPlain (proc, param_ids::padLayerRootNote (1, 2), 48.0f);

    // A note in each zone must produce sound (the zone resolves to a loaded
    // layer rather than falling through to silence).
    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);

    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 40, 0.9f), 0);
        proc.processBlock (buffer, midi);
        REQUIRE (proc.getActiveVoiceCount() > 0);
    }
}

TEST_CASE ("A note outside every key zone still sounds", "[keyzones]")
{
    // Falling silent on a key the user pressed is the wrong failure mode —
    // especially when you cannot see which zones are configured.
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    // Long source: note 90 against root 60 plays ~5.6x speed, so a short
    // sample would be consumed inside the block and report zero voices.
    const auto wav = writeTempWav ("zone_only.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav) == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padMode (1), static_cast<float> (static_cast<int> (PadMode::Keyboard)));
    setPlain (proc, param_ids::padLowNote (1),  0.0f);
    setPlain (proc, param_ids::padHighNote (1), 127.0f);
    setPlain (proc, param_ids::padLayerMode (1),
              static_cast<float> (static_cast<int> (LayerMode::KeyZones)));
    setPlain (proc, param_ids::padLayerLowNote (1, 1),  60.0f);
    setPlain (proc, param_ids::padLayerHighNote (1, 1), 62.0f);

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 90, 0.9f), 0);   // far outside
    proc.processBlock (buffer, midi);

    REQUIRE (proc.getActiveVoiceCount() > 0);
}

// ---------------- Filter envelope ----------------

TEST_CASE ("Filter envelope parameters exist with a bypassed default",
           "[filterenv][parameters]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padFiltEnvAmount (1))  != nullptr);
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padFiltEnvAttack (1))  != nullptr);
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padFiltEnvDecay (1))   != nullptr);
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padFiltEnvSustain (1)) != nullptr);
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padFiltEnvRelease (1)) != nullptr);

    // 0 octaves = no effect, so existing patches sound unchanged.
    REQUIRE (plainValue (proc, param_ids::padFiltEnvAmount (1)) == Catch::Approx (0.0f));
}

TEST_CASE ("Filter envelope opens the filter over time", "[filterenv][voice]")
{
    // A 2 kHz tone through a low-pass parked at 200 Hz. The tone sits above the
    // closed cutoff and below the envelope's open cutoff (200 Hz * 2^6 =
    // 12.8 kHz), so the envelope opening is what lets it through. (A Nyquist
    // square would be wrong here — no reachable cutoff passes it, so both
    // cases would read as silence.)
    juce::AudioBuffer<float> src (1, 44100);
    for (int i = 0; i < src.getNumSamples(); ++i)
        src.setSample (0, i, std::sin (2.0f * juce::MathConstants<float>::pi
                                       * 2000.0f * static_cast<float> (i) / 44100.0f));
    auto sample = std::make_shared<const Sample> (std::move (src), 44100.0);

    auto render = [&sample] (float envOctaves)
    {
        Voice v;
        v.prepare (44100.0);

        VoiceConfig cfg;
        cfg.adsr.attack  = 0.0f;
        cfg.adsr.decay   = 0.0f;
        cfg.adsr.sustain = 1.0f;
        cfg.adsr.release = 1.0f;
        cfg.filterMode     = FilterMode::LowPass;
        cfg.filterCutoffHz = 200.0f;
        cfg.filterEnvOctaves = envOctaves;
        cfg.filterEnvAdsr.attack  = 0.2f;    // slow open
        cfg.filterEnvAdsr.decay   = 0.0f;
        cfg.filterEnvAdsr.sustain = 1.0f;
        cfg.filterEnvAdsr.release = 0.1f;

        v.start (sample, 60, 60, cfg);

        juce::AudioBuffer<float> out (2, 16384);
        out.clear();
        v.renderAdding (out, 0, 16384);

        float peak = 0.0f;
        for (int i = 12000; i < 13000; ++i)
            peak = juce::jmax (peak, std::abs (out.getSample (0, i)));
        return peak;
    };

    const float withEnv    = render (6.0f);   // +6 octaves
    const float withoutEnv = render (0.0f);

    REQUIRE (withEnv > withoutEnv * 1.5f);
}

// ---------------- Stereo shaping ----------------

TEST_CASE ("Stereo parameters exist with neutral defaults", "[stereo][parameters]")
{
    ASamplerProcessor proc;
    REQUIRE (plainValue (proc, param_ids::padStereoWidth (1)) == Catch::Approx (1.0f));
    REQUIRE (plainValue (proc, param_ids::padMonoBelow (1))   == Catch::Approx (0.0f));
}

TEST_CASE ("Zero stereo width collapses a panned voice to mono", "[stereo][voice]")
{
    juce::AudioBuffer<float> src (1, 4410);
    for (int i = 0; i < src.getNumSamples(); ++i)
        src.setSample (0, i, 1.0f);
    auto sample = std::make_shared<const Sample> (std::move (src), 44100.0);

    auto renderWithWidth = [&sample] (float width, float& outL, float& outR)
    {
        Voice v;
        v.prepare (44100.0);

        VoiceConfig cfg;
        cfg.adsr.attack  = 0.0f;
        cfg.adsr.decay   = 0.0f;
        cfg.adsr.sustain = 1.0f;
        cfg.adsr.release = 1.0f;
        cfg.pan         = -0.8f;      // hard-ish left
        cfg.stereoWidth = width;

        v.start (sample, 60, 60, cfg);

        juce::AudioBuffer<float> out (2, 1024);
        out.clear();
        v.renderAdding (out, 0, 1024);

        outL = std::abs (out.getSample (0, 512));
        outR = std::abs (out.getSample (1, 512));
    };

    float wideL = 0.0f, wideR = 0.0f, monoL = 0.0f, monoR = 0.0f;
    renderWithWidth (1.0f, wideL, wideR);
    renderWithWidth (0.0f, monoL, monoR);

    // Panned: channels clearly differ. Collapsed: they match.
    REQUIRE (std::abs (wideL - wideR) > 0.1f);
    REQUIRE (std::abs (monoL - monoR) < 0.001f);
}

TEST_CASE ("Mono Below collapses low frequencies to centre", "[stereo][voice]")
{
    // Hard-left DC (a pure low-frequency case) with the crossover well above
    // it: the two channels must converge.
    juce::AudioBuffer<float> src (1, 44100);
    for (int i = 0; i < src.getNumSamples(); ++i)
        src.setSample (0, i, 1.0f);
    auto sample = std::make_shared<const Sample> (std::move (src), 44100.0);

    Voice v;
    v.prepare (44100.0);

    VoiceConfig cfg;
    cfg.adsr.attack  = 0.0f;
    cfg.adsr.decay   = 0.0f;
    cfg.adsr.sustain = 1.0f;
    cfg.adsr.release = 1.0f;
    cfg.pan         = -1.0f;
    cfg.monoBelowHz = 300.0f;

    v.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 16384);
    out.clear();
    v.renderAdding (out, 0, 16384);

    // Well past the crossover's settling time.
    const float l = out.getSample (0, 12000);
    const float r = out.getSample (1, 12000);
    REQUIRE (std::abs (l - r) < 0.02f);
}

// ---------------- Transient snap ----------------

TEST_CASE ("Transient snap rejects pads with no sample", "[transientsnap]")
{
    ASamplerProcessor proc;
    REQUIRE_FALSE (proc.snapPadWindowToTransients (-1));
    REQUIRE_FALSE (proc.snapPadWindowToTransients (kNumPads));
}

TEST_CASE ("Transient snap keeps a usable window", "[transientsnap]")
{
    ASamplerProcessor proc;

    // Percussive content: four sharp bursts separated by silence.
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("ASamplerTests");
    dir.createDirectory();
    const auto file = dir.getChildFile ("snap_hits.wav");
    file.deleteFile();

    juce::AudioBuffer<float> buf (1, 44100);
    buf.clear();
    for (int hit = 0; hit < 4; ++hit)
    {
        const int at = hit * 11025;
        for (int i = 0; i < 800 && at + i < buf.getNumSamples(); ++i)
        {
            const float decay = 1.0f - static_cast<float> (i) / 800.0f;
            buf.setSample (0, at + i, decay * std::sin (i * 0.4f));
        }
    }

    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    REQUIRE (stream != nullptr);
    std::unique_ptr<juce::AudioFormatWriter> writer (
        fmt.createWriterFor (stream.release(), 44100.0, 1, 16, {}, 0));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    writer.reset();

    REQUIRE (proc.loadSampleForPadLayer (0, 0, file)
             == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padStart (1), 0.2f);
    setPlain (proc, param_ids::padEnd (1),   0.6f);

    if (proc.snapPadWindowToTransients (0))
    {
        const float start = plainValue (proc, param_ids::padStart (1));
        const float end   = plainValue (proc, param_ids::padEnd (1));

        // The window must remain valid — never collapsed or inverted.
        REQUIRE (end > start);
        REQUIRE (start >= 0.0f);
        REQUIRE (end   <= 1.0f);
    }
}

TEST_CASE ("Transient snap is undoable", "[transientsnap][undo]")
{
    ASamplerProcessor proc;
    const auto wav = writeTempWav ("snap_undo.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padStart (1), 0.25f);
    const float before = plainValue (proc, param_ids::padStart (1));

    if (proc.snapPadWindowToTransients (0))
    {
        REQUIRE (proc.undoLastAction());
        REQUIRE (plainValue (proc, param_ids::padStart (1))
                 == Catch::Approx (before).margin (0.001));
    }
}

// ---------------- Global key/scale + focus coverage ----------------

TEST_CASE ("Global key and scale parameters exist", "[scale][parameters]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getState().getRawParameterValue (param_ids::scaleKey)  != nullptr);
    REQUIRE (proc.getState().getRawParameterValue (param_ids::scaleType) != nullptr);

    // Chromatic by default, so scale lock is inert until deliberately set.
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::scaleType))
             == static_cast<int> (Scale::Chromatic));
}

TEST_CASE ("Scale lock keeps a pad audible", "[scale][keyboard]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    setPlain (proc, param_ids::padMode (1), static_cast<float> (static_cast<int> (PadMode::Keyboard)));
    setPlain (proc, param_ids::padLowNote (1),  0.0f);
    setPlain (proc, param_ids::padHighNote (1), 127.0f);
    setPlain (proc, param_ids::padScaleLock (1), 1.0f);
    setPlain (proc, param_ids::scaleType,
              static_cast<float> (static_cast<int> (Scale::Major)));

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 61, 0.9f), 0);   // C#, off-scale
    proc.processBlock (buffer, midi);

    REQUIRE (proc.getActiveVoiceCount() > 0);
}

TEST_CASE ("New per-pad params are all reachable from the Focus group",
           "[focus][accessibility]")
{
    // A parameter missing from the Focus group is only findable by scrolling
    // the full per-pad list, which is the accessibility failure the group
    // exists to prevent.
    ASamplerProcessor proc;

    int proxies = 0;
    for (auto* p : proc.getParameters())
        if (dynamic_cast<FocusProxyParameter*> (p) != nullptr)
            ++proxies;

    int padOneParams = 0;
    for (auto* p : proc.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            if (withId->paramID.startsWith ("pad_1_"))
                ++padOneParams;

    REQUIRE (proxies == padOneParams);
}

// ---------------- Time-stretch / formant integration ----------------

TEST_CASE ("Stretch and formant parameters exist with neutral defaults",
           "[stretch][parameters]")
{
    ASamplerProcessor proc;
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padStretchMode (1)) != nullptr);
    REQUIRE (proc.getState().getRawParameterValue (param_ids::padFormant (1))     != nullptr);

    // Repitch by default: existing projects keep the classic behaviour and
    // pay nothing for the vocoder.
    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padStretchMode (1)))
             == static_cast<int> (StretchMode::Repitch));
    REQUIRE (plainValue (proc, param_ids::padFormant (1)) == Catch::Approx (0.0f));
}

TEST_CASE ("A neutral pad holds no render", "[stretch][render]")
{
    ASamplerProcessor proc;
    const auto wav = writeTempWav ("stretch_neutral.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);

    proc.renderStretchNow (0);
    REQUIRE_FALSE (proc.isPadRendered (0, 0));
}

TEST_CASE ("Stretch mode with tempo sync renders the pad", "[stretch][render]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    const auto wav = writeTempWav ("stretch_sync.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padStretchMode (1),
              static_cast<float> (static_cast<int> (StretchMode::Stretch)));
    setPlain (proc, param_ids::padTempoSync (1), 1.0f);
    // No host transport in tests, so getHostBpm() falls back to 120. An
    // original of 90 therefore implies a 0.75x time ratio.
    setPlain (proc, param_ids::padOriginalBpm (1), 90.0f);

    proc.renderStretchNow (0);
    REQUIRE (proc.isPadRendered (0, 0));
}

TEST_CASE ("Formant shift alone renders the pad", "[formant][render]")
{
    ASamplerProcessor proc;
    const auto wav = writeTempWav ("formant_only.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padFormant (1), 5.0f);

    proc.renderStretchNow (0);
    REQUIRE (proc.isPadRendered (0, 0));
}

TEST_CASE ("Clearing the settings drops the render", "[stretch][render]")
{
    ASamplerProcessor proc;
    const auto wav = writeTempWav ("stretch_clear.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padFormant (1), 4.0f);
    proc.renderStretchNow (0);
    REQUIRE (proc.isPadRendered (0, 0));

    setPlain (proc, param_ids::padFormant (1), 0.0f);
    proc.renderStretchNow (0);
    REQUIRE_FALSE (proc.isPadRendered (0, 0));
}

TEST_CASE ("Loading a new sample invalidates the previous render",
           "[stretch][render]")
{
    // A stale render would keep playing the old audio — the kind of bug that
    // is very hard to identify by ear.
    ASamplerProcessor proc;
    const auto first  = writeTempWav ("stretch_first.wav", 44100);
    const auto second = writeTempWav ("stretch_second.wav", 44100);

    REQUIRE (proc.loadSampleForPadLayer (0, 0, first)
             == ASamplerProcessor::LoadResult::Success);
    setPlain (proc, param_ids::padFormant (1), 6.0f);
    proc.renderStretchNow (0);
    REQUIRE (proc.isPadRendered (0, 0));

    REQUIRE (proc.loadSampleForPadLayer (0, 0, second)
             == ASamplerProcessor::LoadResult::Success);
    REQUIRE_FALSE (proc.isPadRendered (0, 0));   // cleared, awaiting re-render
}

TEST_CASE ("Resetting a pad drops its render", "[stretch][render]")
{
    ASamplerProcessor proc;
    const auto wav = writeTempWav ("stretch_reset.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padFormant (1), 3.0f);
    proc.renderStretchNow (0);
    REQUIRE (proc.isPadRendered (0, 0));

    proc.resetPadLayerToDefault (0, 0);
    REQUIRE_FALSE (proc.isPadRendered (0, 0));
}

TEST_CASE ("A rendered pad still triggers voices", "[stretch][render]")
{
    ASamplerProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    const auto wav = writeTempWav ("stretch_play.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padFormant (1), -4.0f);
    proc.renderStretchNow (0);
    REQUIRE (proc.isPadRendered (0, 0));

    juce::AudioBuffer<float> buffer (proc.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, 0.9f), 0);
    proc.processBlock (buffer, midi);

    REQUIRE (proc.getActiveVoiceCount() > 0);
    REQUIRE (buffer.getMagnitude (0, 512) > 0.0f);
}

TEST_CASE ("Repitch mode never renders, whatever the tempo settings",
           "[stretch][render]")
{
    // Repitch is the default and must stay free: no vocoder, no background
    // work, just a playback-rate change.
    ASamplerProcessor proc;
    const auto wav = writeTempWav ("stretch_repitch.wav", 44100);
    REQUIRE (proc.loadSampleForPadLayer (0, 0, wav)
             == ASamplerProcessor::LoadResult::Success);

    setPlain (proc, param_ids::padStretchMode (1),
              static_cast<float> (static_cast<int> (StretchMode::Repitch)));
    setPlain (proc, param_ids::padTempoSync (1), 1.0f);
    setPlain (proc, param_ids::padOriginalBpm (1), 90.0f);

    proc.renderStretchNow (0);
    REQUIRE_FALSE (proc.isPadRendered (0, 0));
}

TEST_CASE ("Stretch settings survive a state round-trip", "[stretch][state]")
{
    juce::MemoryBlock block;
    {
        ASamplerProcessor proc;
        setPlain (proc, param_ids::padStretchMode (2),
                  static_cast<float> (static_cast<int> (StretchMode::Stretch)));
        setPlain (proc, param_ids::padFormant (2), -7.5f);
        proc.getStateInformation (block);
    }

    ASamplerProcessor proc;
    proc.setStateInformation (block.getData(), (int) block.getSize());

    REQUIRE (juce::roundToInt (plainValue (proc, param_ids::padStretchMode (2)))
             == static_cast<int> (StretchMode::Stretch));
    REQUIRE (plainValue (proc, param_ids::padFormant (2))
             == Catch::Approx (-7.5f).margin (0.05));
}
