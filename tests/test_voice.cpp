#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/FilterMode.h"
#include "engine/LoopMode.h"
#include "engine/Sample.h"
#include "engine/Voice.h"

#include <cmath>
#include <memory>

using namespace asampler;

namespace
{
    // A buffer whose value at index i is i / (N - 1), so the last sample is
    // 1.0 and the magnitude is easy to predict at any point.
    std::shared_ptr<const Sample> makeRamp (int numSamples, double sourceRate = 44100.0)
    {
        juce::AudioBuffer<float> buf (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (0, i, static_cast<float> (i) / static_cast<float> (numSamples - 1));
        return std::make_shared<const Sample> (std::move (buf), sourceRate);
    }

    // A buffer that is 1.0 everywhere — useful for steady-state tests where
    // we don't want the source's own envelope to confuse the assertion.
    std::shared_ptr<const Sample> makeDC (int numSamples, double sourceRate = 44100.0)
    {
        juce::AudioBuffer<float> buf (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (0, i, 1.0f);
        return std::make_shared<const Sample> (std::move (buf), sourceRate);
    }

    // A sine at the given frequency, normalised to amplitude 1.0.
    std::shared_ptr<const Sample> makeSine (int numSamples, double freqHz,
                                            double sourceRate = 44100.0)
    {
        juce::AudioBuffer<float> buf (1, numSamples);
        const double twoPiF = 2.0 * juce::MathConstants<double>::pi * freqHz;
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (0, i, static_cast<float> (std::sin (twoPiF * i / sourceRate)));
        return std::make_shared<const Sample> (std::move (buf), sourceRate);
    }

    float rms (const juce::AudioBuffer<float>& buf, int channel, int start, int len)
    {
        double acc = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double s = buf.getSample (channel, start + i);
            acc += s * s;
        }
        return static_cast<float> (std::sqrt (acc / static_cast<double> (len)));
    }

    VoiceConfig fastEnvelope()
    {
        VoiceConfig cfg;
        cfg.adsr = { 0.0f, 0.0f, 1.0f, 0.0f };  // instant attack, full sustain, instant release
        return cfg;
    }
}

// Equal-power pan centre attenuation: cos(pi/4) = sin(pi/4) = 1/sqrt(2).
// Tests that use the default fastEnvelope() (pan = 0) see this factor on
// both channels — it's the expected -3 dB centre dip, not a bug.
static const float kCentrePanGain = std::sqrt (0.5f);

TEST_CASE ("Voice plays at unity pitch when trigger equals root", "[voice][pitch]")
{
    auto sample = makeRamp (100);

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, fastEnvelope());

    juce::AudioBuffer<float> out (2, 100);
    out.clear();
    voice.renderAdding (out, 0, 100);

    REQUIRE (out.getSample (0, 0) == Catch::Approx (0.0f).margin (1e-5));
    REQUIRE (out.getSample (0, 50)
             == Catch::Approx ((50.0f / 99.0f) * kCentrePanGain).margin (0.01f));
    REQUIRE (out.getSample (0, 98) > 0.95f * kCentrePanGain);
}

TEST_CASE ("Voice pitched up one octave plays sample twice as fast", "[voice][pitch]")
{
    auto sample = makeRamp (200);

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 72, 60, fastEnvelope());

    juce::AudioBuffer<float> out (2, 200);
    out.clear();
    voice.renderAdding (out, 0, 200);

    REQUIRE_FALSE (voice.isActive());

    REQUIRE (out.getSample (0, 50)
             == Catch::Approx (0.5f * kCentrePanGain).margin (0.02f));

    REQUIRE (out.getSample (0, 150) == Catch::Approx (0.0f).margin (1e-5));
}

TEST_CASE ("Voice pan full-left silences right channel", "[voice][pan]")
{
    auto sample = makeDC (50);

    VoiceConfig cfg = fastEnvelope();
    cfg.pan = -1.0f;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 10);
    out.clear();
    voice.renderAdding (out, 0, 10);

    REQUIRE (out.getSample (0, 0) > 0.99f);
    REQUIRE (out.getSample (1, 0) == Catch::Approx (0.0f).margin (1e-5));
}

TEST_CASE ("Voice pan centred yields equal-power gain on both channels", "[voice][pan]")
{
    auto sample = makeDC (50);

    VoiceConfig cfg = fastEnvelope();
    cfg.pan     = 0.0f;
    cfg.gainLin = 1.0f;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 10);
    out.clear();
    voice.renderAdding (out, 0, 10);

    const float expected = std::sqrt (0.5f);
    REQUIRE (out.getSample (0, 0) == Catch::Approx (expected).margin (0.001f));
    REQUIRE (out.getSample (1, 0) == Catch::Approx (expected).margin (0.001f));
}

TEST_CASE ("Voice deactivates when sample exhausted", "[voice][lifetime]")
{
    auto sample = makeRamp (100);

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, fastEnvelope());

    REQUIRE (voice.isActive());

    juce::AudioBuffer<float> out (2, 200);
    out.clear();
    voice.renderAdding (out, 0, 200);

    REQUIRE_FALSE (voice.isActive());
}

TEST_CASE ("Voice release fades to silence and deactivates", "[voice][adsr]")
{
    auto sample = makeDC (44100);

    VoiceConfig cfg;
    cfg.adsr = { 0.0f, 0.0f, 1.0f, 0.005f };

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> warmup (2, 100);
    warmup.clear();
    voice.renderAdding (warmup, 0, 100);
    REQUIRE (voice.isActive());

    voice.release();

    juce::AudioBuffer<float> tail (2, 1000);
    tail.clear();
    voice.renderAdding (tail, 0, 1000);

    REQUIRE_FALSE (voice.isActive());
}

TEST_CASE ("Voice host-rate adjustment compensates for source rate", "[voice][pitch]")
{
    auto sample = makeRamp (200, 48000.0);

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, fastEnvelope());

    juce::AudioBuffer<float> out (2, 200);
    out.clear();
    voice.renderAdding (out, 0, 200);

    REQUIRE_FALSE (voice.isActive());
}

TEST_CASE ("Voice startNorm skips the head of the sample", "[voice][window]")
{
    auto sample = makeRamp (100);  // value = i/99

    VoiceConfig cfg = fastEnvelope();
    cfg.pan       = -1.0f;          // route to channel 0 only — easier to read
    cfg.startNorm = 0.5f;           // start ~halfway through

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 10);
    out.clear();
    voice.renderAdding (out, 0, 10);

    // First emitted sample is the value at index ~49 = 49/99, not 0.
    REQUIRE (out.getSample (0, 0) > 0.45f);
    REQUIRE (out.getSample (0, 0) < 0.55f);
}

TEST_CASE ("Voice endNorm stops playback before the tail", "[voice][window]")
{
    auto sample = makeDC (100);

    VoiceConfig cfg = fastEnvelope();
    cfg.pan     = -1.0f;
    cfg.endNorm = 0.5f;             // window covers ~half the sample

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 100);
    out.clear();
    voice.renderAdding (out, 0, 100);

    // Voice should have deactivated once readPosition reached endIdx-1.
    REQUIRE_FALSE (voice.isActive());

    // Beyond the window we expect zero output.
    REQUIRE (out.getSample (0, 60) == Catch::Approx (0.0f).margin (1e-5));
    REQUIRE (out.getSample (0, 99) == Catch::Approx (0.0f).margin (1e-5));
}

TEST_CASE ("Voice reverse walks the sample backwards", "[voice][reverse]")
{
    auto sample = makeRamp (100);   // 0..1 forward

    VoiceConfig cfg = fastEnvelope();
    cfg.pan     = -1.0f;
    cfg.reverse = true;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 100);
    out.clear();
    voice.renderAdding (out, 0, 100);

    // First emitted sample should be near the top of the ramp (~1.0).
    REQUIRE (out.getSample (0, 0) > 0.95f);
    // Halfway through we should be roughly mid-ramp.
    REQUIRE (out.getSample (0, 50) > 0.45f);
    REQUIRE (out.getSample (0, 50) < 0.55f);
}

TEST_CASE ("Voice reverse honours start/end window", "[voice][reverse][window]")
{
    auto sample = makeRamp (100);

    VoiceConfig cfg = fastEnvelope();
    cfg.pan       = -1.0f;
    cfg.reverse   = true;
    cfg.startNorm = 0.25f;
    cfg.endNorm   = 0.75f;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 200);
    out.clear();
    voice.renderAdding (out, 0, 200);

    // First sample should be near endIdx-1 = ~73/99 of a ramp 0..1.
    REQUIRE (out.getSample (0, 0) > 0.65f);
    REQUIRE (out.getSample (0, 0) < 0.80f);

    // After the window has been consumed in reverse, the voice deactivates.
    REQUIRE_FALSE (voice.isActive());
}

TEST_CASE ("Voice keeps source alive after the owning shared_ptr is reset", "[voice][lifetime]")
{
    // Simulates pad-sample-swap: load a sample, start a voice on it, then
    // drop the original owner. The voice's own shared_ptr keeps the data alive.
    Voice voice;
    voice.prepare (44100.0);

    {
        auto sample = makeDC (100);
        voice.start (sample, 60, 60, fastEnvelope());
        // `sample` goes out of scope here; voice should still hold a strong ref.
    }

    juce::AudioBuffer<float> out (2, 50);
    out.clear();
    voice.renderAdding (out, 0, 50);

    // If the source were prematurely freed, this would either crash or render
    // garbage. Centre-panned DC should give kCentrePanGain on both channels.
    REQUIRE (out.getSample (0, 0) == Catch::Approx (kCentrePanGain).margin (0.001f));
    REQUIRE (out.getSample (1, 0) == Catch::Approx (kCentrePanGain).margin (0.001f));
}

TEST_CASE ("Voice filter Off leaves the signal unchanged", "[voice][filter]")
{
    // Regression guard: when filterMode is Off, output must match the
    // pre-filter-feature path exactly. DC at -3 dB centre pan.
    auto sample = makeDC (200);

    VoiceConfig cfg = fastEnvelope();
    cfg.filterMode = FilterMode::Off;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 50);
    out.clear();
    voice.renderAdding (out, 0, 50);

    REQUIRE (out.getSample (0, 25) == Catch::Approx (kCentrePanGain).margin (0.001f));
    REQUIRE (out.getSample (1, 25) == Catch::Approx (kCentrePanGain).margin (0.001f));
}

TEST_CASE ("Voice low-pass at 200 Hz attenuates an 8 kHz sine", "[voice][filter]")
{
    auto sample = makeSine (4096, 8000.0);

    VoiceConfig cfg = fastEnvelope();
    cfg.pan             = -1.0f;            // route to channel 0 only
    cfg.filterMode      = FilterMode::LowPass;
    cfg.filterCutoffHz  = 200.0f;
    cfg.filterResonance = 0.707f;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 4096);
    out.clear();
    voice.renderAdding (out, 0, 4096);

    // Skip the first ~5 ms so the filter has settled before measuring.
    const int skip = 256;
    const float outRms = rms (out, 0, skip, 4096 - skip);

    // SVF at 8 kHz with cutoff 200 Hz is ~32 octaves above cutoff; -12 dB/oct
    // takes the level well below the source's 0.707 RMS.
    REQUIRE (outRms < 0.05f);
}

TEST_CASE ("Voice high-pass kills DC", "[voice][filter]")
{
    auto sample = makeDC (4096);

    VoiceConfig cfg = fastEnvelope();
    cfg.pan             = -1.0f;
    cfg.filterMode      = FilterMode::HighPass;
    cfg.filterCutoffHz  = 1000.0f;
    cfg.filterResonance = 0.707f;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 4096);
    out.clear();
    voice.renderAdding (out, 0, 4096);

    // After the transient settles, a high-pass on pure DC must be near zero.
    const float tail = rms (out, 0, 2048, 2048);
    REQUIRE (tail < 0.01f);
}

TEST_CASE ("Voice uses 4-point cubic Hermite interpolation", "[voice][interp]")
{
    // Impulse-like content: a single 1.0 surrounded by zeros. At a half-step
    // read position between the zero-before and the impulse, linear interp
    // gives exactly 0.5; Catmull-Rom cubic Hermite gives 0.5625. The latter
    // is the only one that can produce > 0.5 here.
    //
    // pitchRatio comes from sourceRate / hostRate = 66150 / 44100 = 1.5, so
    // host sample i lands at source position i * 1.5.
    juce::AudioBuffer<float> buf (1, 10);
    buf.clear();
    buf.setSample (0, 2, 1.0f);  // pattern: 0, 0, 1, 0, 0, ...

    auto sample = std::make_shared<const Sample> (std::move (buf), 66150.0);

    VoiceConfig cfg = fastEnvelope();
    cfg.pan = -1.0f;             // route to channel 0 only

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 4);
    out.clear();
    voice.renderAdding (out, 0, 4);

    // i=0: readPosition 0.0, frac 0 -> y1 = 0.
    REQUIRE (out.getSample (0, 0) == Catch::Approx (0.0f).margin (1e-5));

    // i=1: readPosition 1.5, idx=1, frac=0.5. Analytic Catmull-Rom output
    // for (y0=0, y1=0, y2=1, y3=0) at t=0.5 is 0.5625.
    REQUIRE (out.getSample (0, 1) == Catch::Approx (0.5625f).margin (0.001f));

    // Strict cubic-vs-linear check: linear interp could never reach > 0.5
    // here, because the bounding samples are 0 and 1.
    REQUIRE (out.getSample (0, 1) > 0.55f);
}

TEST_CASE ("Voice forward loop replays the sample window", "[voice][loop]")
{
    // 100-sample ramp 0..1. Forward loop covering the whole sample.
    // Render 250 samples — without a loop we'd deactivate around sample 99.
    auto sample = makeRamp (100);

    VoiceConfig cfg = fastEnvelope();
    cfg.pan      = -1.0f;
    cfg.loopMode = LoopMode::Forward;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 250);
    out.clear();
    voice.renderAdding (out, 0, 250);

    // Still active long after the one-shot would have died.
    REQUIRE (voice.isActive());

    // Each lap reaches near the top of the ramp.
    REQUIRE (out.getSample (0, 95)  > 0.90f);  // first lap, near end
    REQUIRE (out.getSample (0, 195) > 0.90f);  // second lap, near end
}

TEST_CASE ("Voice ping-pong loop flips direction at bounds", "[voice][loop]")
{
    // 100-sample ramp. Ping-pong over [25..75). Direction should reverse
    // each time we hit a boundary, so the output trajectory goes up, down,
    // up, ... .
    auto sample = makeRamp (100);

    VoiceConfig cfg = fastEnvelope();
    cfg.pan           = -1.0f;
    cfg.loopMode      = LoopMode::PingPong;
    cfg.loopStartNorm = 0.25f;
    cfg.loopEndNorm   = 0.75f;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    // ~50 samples per leg at unity pitch. Render 200 to cover 4 legs.
    juce::AudioBuffer<float> out (2, 200);
    out.clear();
    voice.renderAdding (out, 0, 200);

    REQUIRE (voice.isActive());

    // After the first reflection (~sample 73), the voice plays the ramp
    // backwards. Within the reverse leg the value must be monotonically
    // decreasing — pick two samples comfortably inside that leg.
    const float earlyReverse = out.getSample (0, 80);   // just past the bounce
    const float lateReverse  = out.getSample (0, 110);  // deeper into reverse
    REQUIRE (lateReverse < earlyReverse);
}

TEST_CASE ("Voice loop ends when envelope release completes", "[voice][loop][adsr]")
{
    // Loop should not keep a voice alive forever — release on the envelope
    // must still tear it down.
    auto sample = makeDC (1000);

    VoiceConfig cfg;
    cfg.adsr     = { 0.0f, 0.0f, 1.0f, 0.005f };  // 5 ms release
    cfg.loopMode = LoopMode::Forward;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> warmup (2, 100);
    warmup.clear();
    voice.renderAdding (warmup, 0, 100);
    REQUIRE (voice.isActive());

    voice.release();

    juce::AudioBuffer<float> tail (2, 2000);
    tail.clear();
    voice.renderAdding (tail, 0, 2000);

    REQUIRE_FALSE (voice.isActive());
}

TEST_CASE ("Voice loop off matches one-shot behaviour", "[voice][loop]")
{
    // Regression: with LoopMode::Off, voice deactivates once the sample is
    // exhausted, exactly like before the loop feature existed.
    auto sample = makeRamp (50);

    VoiceConfig cfg = fastEnvelope();
    cfg.loopMode = LoopMode::Off;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, cfg);

    juce::AudioBuffer<float> out (2, 200);
    out.clear();
    voice.renderAdding (out, 0, 200);

    REQUIRE_FALSE (voice.isActive());
}

TEST_CASE ("Voice filter state resets on retrigger", "[voice][filter][lifetime]")
{
    // Start with a high-Q resonant LP on DC to charge the filter, then
    // retrigger with the filter Off. The first emitted sample must be the
    // clean DC value (no leftover ring from the previous run).
    auto sample = makeDC (1024);

    VoiceConfig hot = fastEnvelope();
    hot.pan             = -1.0f;
    hot.filterMode      = FilterMode::LowPass;
    hot.filterCutoffHz  = 100.0f;
    hot.filterResonance = 8.0f;

    Voice voice;
    voice.prepare (44100.0);
    voice.start (sample, 60, 60, hot);

    juce::AudioBuffer<float> warmup (2, 256);
    warmup.clear();
    voice.renderAdding (warmup, 0, 256);

    // Retrigger with filter off; expect immediate, clean output.
    VoiceConfig cold = fastEnvelope();
    cold.pan        = -1.0f;
    cold.filterMode = FilterMode::Off;
    voice.start (sample, 60, 60, cold);

    juce::AudioBuffer<float> out (2, 16);
    out.clear();
    voice.renderAdding (out, 0, 16);

    REQUIRE (out.getSample (0, 0) == Catch::Approx (1.0f).margin (0.001f));
}
