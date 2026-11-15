#include "StarterSounds.h"

#include <cmath>

namespace asampler::starter
{
    namespace
    {
        constexpr double kTwoPi = juce::MathConstants<double>::twoPi;

        juce::AudioBuffer<float> makeBuffer (double seconds, double sr)
        {
            juce::AudioBuffer<float> b (1, juce::jmax (1, (int) std::ceil (seconds * sr)));
            b.clear();
            return b;
        }

        // Sine whose frequency glides exponentially from f0 to f1 with time
        // constant `sweep` seconds, amplitude decaying with `decay` seconds.
        void addSweptSine (juce::AudioBuffer<float>& b, double sr, double f0, double f1, double sweep,
                           double decay, float gain, double drive = 0.0)
        {
            auto* d = b.getWritePointer (0);
            double phase = 0.0;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const double t = i / sr;
                const double f = f1 + (f0 - f1) * std::exp (-t / sweep);
                phase += kTwoPi * f / sr;
                double v = std::sin (phase) * std::exp (-t / decay);
                if (drive > 0.0)
                    v = std::tanh (v * (1.0 + drive)) / std::tanh (1.0 + drive);
                d[i] += gain * (float) v;
            }
        }

        // Filtered noise with an exponential decay, starting at `offset` s.
        void addNoise (juce::AudioBuffer<float>& b, double sr, juce::IIRCoefficients coeffs, double decay,
                       float gain, int seed, double offset = 0.0, double attack = 0.0005)
        {
            juce::Random rng (seed);
            juce::IIRFilter filter;
            filter.setCoefficients (coeffs);
            auto* d = b.getWritePointer (0);
            const int start = (int) (offset * sr);
            for (int i = start; i < b.getNumSamples(); ++i)
            {
                const double t = (i - start) / sr;
                const float  n = filter.processSingleSampleRaw (rng.nextFloat() * 2.0f - 1.0f);
                const double env = std::exp (-t / decay) * juce::jmin (1.0, t / attack);
                d[i] += gain * n * (float) env;
            }
        }

        void finish (juce::AudioBuffer<float>& b, double sr)
        {
            // A 2 ms fade at the end, then peak at -1 dBFS.
            const int fade = juce::jmin (b.getNumSamples(), (int) (0.002 * sr));
            b.applyGainRamp (0, b.getNumSamples() - fade, fade, 1.0f, 0.0f);
            const float peak = b.getMagnitude (0, b.getNumSamples());
            if (peak > 0.0f)
                b.applyGain (juce::Decibels::decibelsToGain (-1.0f) / peak);
        }
    }

    juce::StringArray soundNames()
    {
        return { "Kick", "Closed Hat", "Open Hat", "Snare", "Clap", "Rim", "Tom", "808" };
    }

    juce::AudioBuffer<float> render (int index, double sr)
    {
        using C = juce::IIRCoefficients;
        juce::AudioBuffer<float> b;

        switch (index)
        {
            case 0:   // Kick: pitch drop, short click
                b = makeBuffer (0.9, sr);
                addSweptSine (b, sr, 160.0, 48.0, 0.035, 0.22, 1.0f, 0.6);
                addNoise (b, sr, C::makeBandPass (sr, 3500.0, 0.8), 0.004, 0.25f, 11);
                break;

            case 1:   // Closed hat
                b = makeBuffer (0.12, sr);
                addNoise (b, sr, C::makeHighPass (sr, 7500.0, 0.9), 0.028, 1.0f, 21);
                break;

            case 2:   // Open hat
                b = makeBuffer (1.0, sr);
                addNoise (b, sr, C::makeHighPass (sr, 7000.0, 0.9), 0.22, 1.0f, 31);
                break;

            case 3:   // Snare: body plus a band of noise
                b = makeBuffer (0.55, sr);
                addSweptSine (b, sr, 240.0, 185.0, 0.02, 0.07, 0.7f);
                addSweptSine (b, sr, 340.0, 330.0, 0.02, 0.05, 0.3f);
                addNoise (b, sr, C::makeBandPass (sr, 4200.0, 0.6), 0.11, 0.9f, 41);
                break;

            case 4:   // Clap: three quick bursts and a tail
                b = makeBuffer (0.6, sr);
                for (int k = 0; k < 3; ++k)
                    addNoise (b, sr, C::makeBandPass (sr, 1300.0, 1.2), 0.008, 0.9f, 51 + k, k * 0.011);
                addNoise (b, sr, C::makeBandPass (sr, 1300.0, 1.0), 0.12, 0.7f, 55, 0.033);
                break;

            case 5:   // Rim
                b = makeBuffer (0.12, sr);
                addSweptSine (b, sr, 1750.0, 1700.0, 0.01, 0.012, 0.6f);
                addSweptSine (b, sr, 820.0, 800.0, 0.01, 0.018, 0.5f);
                addNoise (b, sr, C::makeHighPass (sr, 2000.0, 0.7), 0.004, 0.4f, 61);
                break;

            case 6:   // Tom
                b = makeBuffer (0.9, sr);
                addSweptSine (b, sr, 150.0, 98.0, 0.06, 0.2, 1.0f, 0.3);
                addNoise (b, sr, C::makeBandPass (sr, 2500.0, 0.8), 0.006, 0.15f, 71);
                break;

            case 7:   // 808: long sub with a touch of drive, tuned to A1
                b = makeBuffer (2.6, sr);
                addSweptSine (b, sr, 110.0, 55.0, 0.025, 0.55, 1.0f, 1.2);
                break;

            default:
                return {};
        }

        finish (b, sr);
        return b;
    }
}
