#include "Voice.h"
#include "Sample.h"
#include "Lfo.h"
#include "Saturation.h"

#include <cmath>

namespace asampler
{
    namespace
    {
        // Nudge `idx` to the nearest sample (within `maxSearch`) where the
        // mono-summed waveform crosses zero, staying inside [lo, hi]. Used to
        // silence the click that a start/end/loop boundary in the middle of a
        // waveform would otherwise produce. Falls back to the original index
        // when no crossing is found nearby.
        int snapIndexToZeroCrossing (const float* const* channels, int numChannels,
                                     int idx, int lo, int hi, int maxSearch) noexcept
        {
            idx = juce::jlimit (lo, hi, idx);
            if (numChannels <= 0 || hi <= lo)
                return idx;

            auto monoAt = [channels, numChannels] (int i) noexcept
            {
                float s = 0.0f;
                for (int c = 0; c < numChannels; ++c)
                    s += channels[c][i];
                return s;
            };

            const float here = monoAt (idx);
            if (here == 0.0f)
                return idx;

            for (int d = 1; d <= maxSearch; ++d)
            {
                const int up = idx + d;
                if (up < hi && (monoAt (up) == 0.0f || (monoAt (up) * monoAt (up - 1)) < 0.0f))
                    return up;

                const int dn = idx - d;
                if (dn > lo && (monoAt (dn) == 0.0f || (monoAt (dn) * monoAt (dn + 1)) < 0.0f))
                    return dn;
            }
            return idx;
        }
    }

    void Voice::prepare (double sr)
    {
        hostRate = sr;
        envelope.setSampleRate (sr);

        filterEnv.setSampleRate (sr);

        smoothedGain  .reset (sr, kSmoothingSeconds);
        smoothedPan   .reset (sr, kSmoothingSeconds);
        smoothedCutoff.reset (sr, kSmoothingSeconds);

        juce::dsp::ProcessSpec spec { sr, /*maxBlockSize*/ 1, /*numChannels*/ 2 };
        filter.prepare (spec);
        filter.reset();
    }

    void Voice::start (std::shared_ptr<const Sample> src,
                       int triggerNote,
                       int rootNote,
                       const VoiceConfig& cfg)
    {
        source = std::move (src);

        const double semitones = static_cast<double> (triggerNote - rootNote)
                               + static_cast<double> (cfg.pitchSemitones);
        const double pitchScale = std::pow (2.0, semitones / 12.0);
        const double srRatio    = (source != nullptr ? source->getSourceSampleRate() : hostRate) / hostRate;
        basePitchRatio          = pitchScale * srRatio;
        pitchBendSemitones      = cfg.pitchBendSemitones;
        pitchBendScale          = std::pow (2.0, static_cast<double> (cfg.pitchBendSemitones) / 12.0);
        direction               = cfg.reverse ? -1.0 : 1.0;

        // Initialise glide. If either input is zero/non-positive, glide is
        // off and the voice plays at its natural pitch from sample zero.
        if (cfg.glideStartSemitones != 0.0f && cfg.glideTimeMs > 0.0f)
        {
            glideSemitones        = static_cast<double> (cfg.glideStartSemitones);
            glideRemainingSamples = juce::jmax (1, juce::roundToInt (
                                                       static_cast<double> (cfg.glideTimeMs)
                                                       * 0.001 * hostRate));
            // Walk the semitone offset linearly to zero, converting to a ratio
            // each sample. Constant musical rate, unlike lerping the ratio.
            glideSemitoneStep     = -glideSemitones / static_cast<double> (glideRemainingSamples);
            glideScale            = std::pow (2.0, glideSemitones / 12.0);
        }
        else
        {
            glideSemitones        = 0.0;
            glideSemitoneStep     = 0.0;
            glideScale            = 1.0;
            glideRemainingSamples = 0;
        }

        updatePitchRatio();

        // Clamp start/end into a usable window. Linear interpolation needs
        // idx+1 in range, so the last addressable frame is srcSamples-2.
        const int srcSamples = (source != nullptr) ? source->getNumSamples() : 0;
        if (srcSamples >= 2)
        {
            const float s = juce::jlimit (0.0f, 1.0f, cfg.startNorm);
            const float e = juce::jlimit (0.0f, 1.0f, cfg.endNorm);
            startIdx = juce::jlimit (0, srcSamples - 2,
                                     static_cast<int> (s * static_cast<float> (srcSamples - 1)));
            endIdx   = juce::jlimit (startIdx + 1, srcSamples - 1,
                                     static_cast<int> (e * static_cast<float> (srcSamples - 1)));

            // Optional zero-crossing snap on the window boundaries. Search a
            // few ms either side so the nudge is inaudible in position but
            // removes the boundary click. Keep endIdx strictly above startIdx.
            if (cfg.snapToZeroCrossings && source != nullptr)
            {
                const int maxSearch = juce::jmax (1, static_cast<int> (0.003 * hostRate));
                const float* chans[2] = { source->getReadPointer (0),
                                          (source->getNumChannels() >= 2)
                                              ? source->getReadPointer (1)
                                              : source->getReadPointer (0) };
                const int nch = juce::jmin (2, source->getNumChannels());
                startIdx = snapIndexToZeroCrossing (chans, nch, startIdx,
                                                    0, srcSamples - 2, maxSearch);
                endIdx   = snapIndexToZeroCrossing (chans, nch, endIdx,
                                                    startIdx + 1, srcSamples - 1, maxSearch);
            }
        }
        else
        {
            startIdx = 0;
            endIdx   = srcSamples;
        }

        // Reverse playback walks from just inside the upper bound back toward startIdx.
        readPosition = cfg.reverse ? static_cast<double> (endIdx - 1)
                                   : static_cast<double> (startIdx);

        // Loop bounds, clamped to fit inside the playback window. If the user's
        // loop region collapses (start >= end), fall back to looping the whole
        // window so we never end up with a zero-width loop.
        loopMode = cfg.loopMode;
        if (loopMode != LoopMode::Off && endIdx > startIdx + 1)
        {
            const float ls = juce::jlimit (0.0f, 1.0f, cfg.loopStartNorm);
            const float le = juce::jlimit (0.0f, 1.0f, cfg.loopEndNorm);
            const auto windowFloat = static_cast<float> (srcSamples - 1);
            int candidateStart = static_cast<int> (ls * windowFloat);
            int candidateEnd   = static_cast<int> (le * windowFloat);

            loopStartIdx = juce::jlimit (startIdx, endIdx - 2, candidateStart);
            loopEndIdx   = juce::jlimit (loopStartIdx + 1, endIdx - 1, candidateEnd);
        }
        else
        {
            loopMode     = LoopMode::Off;
            loopStartIdx = startIdx;
            loopEndIdx   = endIdx;
        }

        // Equal-power pan. -3 dB in the centre — standard pro-audio convention.
        const float pan   = juce::jlimit (-1.0f, 1.0f, cfg.pan);
        const float angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        gainL = cfg.gainLin * std::cos (angle);
        gainR = cfg.gainLin * std::sin (angle);
        baseGainLin = cfg.gainLin;   // kept for per-sample LFO volume/pan recompute
        basePan     = pan;

        // Snap the ramps to the trigger values — a new voice must start at its
        // own level, never slide up from whatever the previous voice left.
        smoothedGain.setCurrentAndTargetValue (cfg.gainLin);
        smoothedPan .setCurrentAndTargetValue (pan);
        lastSmoothedGain = cfg.gainLin;
        lastSmoothedPan  = pan;

        // Remember this hit's velocity offsets so live pushes can re-apply them.
        hitGainScale        = (cfg.hitGainScale        > 0.0f) ? cfg.hitGainScale        : 1.0f;
        hitCutoffMultiplier = (cfg.hitCutoffMultiplier > 0.0f) ? cfg.hitCutoffMultiplier : 1.0f;

        envelope.setParameters (cfg.adsr);
        envelope.reset();
        envelope.noteOn();

        // Filter envelope. Gated on a non-zero amount so the common case pays
        // nothing per sample.
        filterEnvOctaves = cfg.filterEnvOctaves;
        filterEnvActive  = (cfg.filterMode != FilterMode::Off)
                        && (std::abs (filterEnvOctaves) > 0.0001f);
        filterEnv.setParameters (cfg.filterEnvAdsr);
        filterEnv.reset();
        if (filterEnvActive)
            filterEnv.noteOn();

        // Stereo shaping. Skip the whole path when width is unity and the
        // mono-below crossover is off.
        stereoWidth   = juce::jlimit (0.0f, 2.0f, cfg.stereoWidth);
        monoBelowHz   = juce::jmax (0.0f, cfg.monoBelowHz);
        stereoShaping = (std::abs (stereoWidth - 1.0f) > 0.001f) || (monoBelowHz > 0.0f);
        monoLpStateL  = 0.0f;
        monoLpStateR  = 0.0f;
        // One-pole low-pass coefficient for the crossover corner.
        monoLpCoeff   = (monoBelowHz > 0.0f && hostRate > 0.0)
                          ? static_cast<float> (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi
                                                                * monoBelowHz / hostRate))
                          : 0.0f;

        aftertouchGainMultiplier   = cfg.aftertouchVolumeMultiplier;
        aftertouchCutoffMultiplier = cfg.aftertouchCutoffMultiplier;
        padGate                    = cfg.padGate;

        // Configure per-voice filter. Reset clears state so retrigger does not
        // bleed the previous note's tail through the filter.
        filterMode    = cfg.filterMode;
        baseCutoffHz  = cfg.filterCutoffHz;
        smoothedCutoff.setCurrentAndTargetValue (cfg.filterCutoffHz);
        lastCutoffSet   = -1.0f;   // force the first setCutoffFrequency below
        currentResonance = juce::jlimit (0.1f, 10.0f, cfg.filterResonance);
        if (filterMode != FilterMode::Off)
        {
            using SVF = juce::dsp::StateVariableTPTFilter<float>;
            switch (filterMode)
            {
                case FilterMode::LowPass:  filter.setType (SVF::Type::lowpass);  break;
                case FilterMode::HighPass: filter.setType (SVF::Type::highpass); break;
                case FilterMode::BandPass: filter.setType (SVF::Type::bandpass); break;
                case FilterMode::Off:                                             break;
            }
            updateFilterCutoff();
            filter.setResonance (currentResonance);
            filter.reset();
        }

        // Pitch envelope. Starts at 2^(amount/12) and lerps in ratio-space back
        // to 1.0 over the decay time. Zero amount or time leaves it at unity.
        if (cfg.pitchEnvSemitones != 0.0f && cfg.pitchEnvTimeMs > 0.0f)
        {
            pitchEnvScale             = std::pow (2.0,
                                                  static_cast<double> (cfg.pitchEnvSemitones) / 12.0);
            pitchEnvRemainingSamples  = juce::jmax (1, juce::roundToInt (
                                                        static_cast<double> (cfg.pitchEnvTimeMs)
                                                        * 0.001 * hostRate));
            pitchEnvIncrement         = (1.0 - pitchEnvScale)
                                          / static_cast<double> (pitchEnvRemainingSamples);
        }
        else
        {
            pitchEnvScale            = 1.0;
            pitchEnvRemainingSamples = 0;
            pitchEnvIncrement        = 0.0;
        }

        updatePitchRatio();

        // Per-pad LFO. Precompute the phase increment; gate the whole path on
        // a live destination + non-zero depth so the no-LFO case is cheap.
        lfoShape    = cfg.lfoShape;
        lfoDest     = cfg.lfoDest;
        lfoDepth    = juce::jlimit (0.0f, 1.0f, cfg.lfoDepth);
        lfoPhase    = 0.0;
        lfoPhaseInc = (hostRate > 0.0) ? (static_cast<double> (cfg.lfoRateHz) / hostRate) : 0.0;
        lfoActive   = (lfoDest != LfoDest::Off) && (lfoDepth > 0.0f) && (lfoPhaseInc > 0.0);

        // Saturation / bit-crush character chain.
        satMode          = cfg.satMode;
        satAmount        = juce::jlimit (0.0f, 1.0f, cfg.satAmount);
        bitDepth         = cfg.bitDepth;
        downsampleFactor = juce::jmax (1, cfg.downsampleFactor);
        crushCounter     = 0;
        crushHoldL       = 0.0f;
        crushHoldR       = 0.0f;
        characterActive  = (satAmount > 0.0f) || (bitDepth < 16) || (downsampleFactor > 1);

        // Edge fades + loop crossfade, converted to samples and clamped so they
        // can't exceed the available window / loop span.
        const int windowLen = juce::jmax (0, endIdx - startIdx);
        fadeInSamples  = juce::jlimit (0, windowLen,
                                       juce::roundToInt (cfg.fadeInMs  * 0.001f * static_cast<float> (hostRate)));
        fadeOutSamples = juce::jlimit (0, windowLen,
                                       juce::roundToInt (cfg.fadeOutMs * 0.001f * static_cast<float> (hostRate)));
        if (loopMode == LoopMode::Forward)
        {
            const int loopSpan = juce::jmax (0, loopEndIdx - loopStartIdx);
            loopXfadeSamples = juce::jlimit (0, loopSpan,
                                             juce::roundToInt (cfg.loopXfadeMs * 0.001f * static_cast<float> (hostRate)));
        }
        else
        {
            loopXfadeSamples = 0;
        }

        active = (source != nullptr) && ! source->isEmpty() && endIdx > startIdx;
    }

    void Voice::release() noexcept
    {
        envelope.noteOff();
        filterEnv.noteOff();   // both envelopes release together
    }

    void Voice::setPitchBendSemitones (float semitones) noexcept
    {
        pitchBendSemitones = semitones;
        pitchBendScale     = std::pow (2.0, static_cast<double> (semitones) / 12.0);
        updatePitchRatio();
    }

    void Voice::setAftertouchModulation (float volumeMultiplier,
                                         float cutoffMultiplier) noexcept
    {
        aftertouchGainMultiplier   = volumeMultiplier;
        aftertouchCutoffMultiplier = cutoffMultiplier;
        if (filterMode != FilterMode::Off)
            updateFilterCutoff();
    }

    void Voice::setLiveParams (const VoiceLiveParams& live) noexcept
    {
        if (! active)
            return;

        // Re-apply this voice's own per-hit velocity offsets on top of the
        // pad's base values, so a live push tracks the knob without flattening
        // the velocity response the hit was triggered with.
        smoothedGain  .setTargetValue (live.gainLin * hitGainScale);
        smoothedPan   .setTargetValue (juce::jlimit (-1.0f, 1.0f, live.pan));
        smoothedCutoff.setTargetValue (live.filterCutoffHz * hitCutoffMultiplier);

        // Resonance changes filter coefficients directly; only touch it when it
        // actually moved, since setResonance recomputes coefficients.
        const float res = juce::jlimit (0.1f, 10.0f, live.filterResonance);
        if (res != currentResonance)
        {
            currentResonance = res;
            if (filterMode != FilterMode::Off)
                filter.setResonance (res);
        }

        // Saturation amount and LFO rate/depth are continuous and cheap; a step
        // change in them is not audible as a click, so no ramp is needed.
        satAmount       = juce::jlimit (0.0f, 1.0f, live.satAmount);
        characterActive = (satAmount > 0.0f) || (bitDepth < 16) || (downsampleFactor > 1);

        lfoDepth    = juce::jlimit (0.0f, 1.0f, live.lfoDepth);
        lfoPhaseInc = (hostRate > 0.0) ? (static_cast<double> (live.lfoRateHz) / hostRate) : 0.0;
        lfoActive   = (lfoDest != LfoDest::Off) && (lfoDepth > 0.0f) && (lfoPhaseInc > 0.0);

        // Filter-envelope depth is live; its ADSR shape stays trigger-time,
        // like the amp envelope.
        filterEnvOctaves = live.filterEnvOctaves;
        filterEnvActive  = (filterMode != FilterMode::Off)
                        && (std::abs (filterEnvOctaves) > 0.0001f);

        stereoWidth   = juce::jlimit (0.0f, 2.0f, live.stereoWidth);
        monoBelowHz   = juce::jmax (0.0f, live.monoBelowHz);
        stereoShaping = (std::abs (stereoWidth - 1.0f) > 0.001f) || (monoBelowHz > 0.0f);
        monoLpCoeff   = (monoBelowHz > 0.0f && hostRate > 0.0)
                          ? static_cast<float> (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi
                                                                * monoBelowHz / hostRate))
                          : 0.0f;
    }

    void Voice::updatePitchRatio() noexcept
    {
        pitchRatio = basePitchRatio * pitchBendScale * glideScale * pitchEnvScale * direction;
    }

    void Voice::updateFilterCutoff() noexcept
    {
        const float nyquist = static_cast<float> (hostRate * 0.5);
        const float target  = baseCutoffHz * aftertouchCutoffMultiplier;
        lastCutoffSet = target;   // keep the render loop's change detector in sync
        filter.setCutoffFrequency (juce::jlimit (20.0f, nyquist - 1.0f, target));
    }

    void Voice::stop() noexcept
    {
        envelope.reset();
        active = false;
        source.reset();
    }

    void Voice::renderAdding (juce::AudioBuffer<float>& output, int startSample, int numSamples)
    {
        if (! active || source == nullptr)
            return;

        jassert (output.getNumChannels() >= 2);

        const int    srcChannels = source->getNumChannels();
        const float* srcL = source->getReadPointer (0);
        const float* srcR = (srcChannels >= 2) ? source->getReadPointer (1) : srcL;

        // 4-point Catmull-Rom cubic Hermite.
        auto hermite = [] (float frac, float y0, float y1, float y2, float y3)
        {
            const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            const float b =         y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c = -0.5f * y0                          + 0.5f * y2;
            const float d =                    y1;
            return ((a * frac + b) * frac + c) * frac + d;
        };

        // Interpolated read at an arbitrary position. Neighbour indices are
        // clamped against the playback window so the interpolator never reads
        // outside [startIdx, endIdx). Used for the main read and for the
        // loop-crossfade partner read.
        auto readAt = [&] (double pos, float& oL, float& oR)
        {
            int i0 = juce::jlimit (startIdx, endIdx - 1, static_cast<int> (std::floor (pos)));
            const float fr  = juce::jlimit (0.0f, 1.0f, static_cast<float> (pos - i0));
            const int   im1 = (i0 > startIdx)     ? i0 - 1 : i0;
            const int   ip1 = (i0 + 1 < endIdx)   ? i0 + 1 : i0;
            const int   ip2 = (i0 + 2 < endIdx)   ? i0 + 2 : ip1;
            oL = hermite (fr, srcL[im1], srcL[i0], srcL[ip1], srcL[ip2]);
            oR = hermite (fr, srcR[im1], srcR[i0], srcR[ip1], srcR[ip2]);
        };

        for (int i = 0; i < numSamples; ++i)
        {
            // Apply loop wrap/bounce BEFORE the bounds check so the voice
            // doesn't deactivate when readPosition naturally crosses a loop
            // boundary. Only the sample-window bounds [startIdx, endIdx) can
            // kill the voice on their own.
            if (loopMode != LoopMode::Off)
            {
                const double loopSpan = static_cast<double> (loopEndIdx - loopStartIdx);

                if (loopMode == LoopMode::Forward)
                {
                    while (readPosition >= loopEndIdx)        readPosition -= loopSpan;
                    while (readPosition <  loopStartIdx)      readPosition += loopSpan;
                }
                else // PingPong
                {
                    // Reflect at upper bound: overshoot beyond (loopEndIdx-1)
                    // bounces back, and direction flips. Flipping `direction`
                    // (rather than pitchRatio directly) keeps the bend offset
                    // safe across the reversal — a live setPitchBendSemitones
                    // re-derives pitchRatio from the *current* direction.
                    if (readPosition >= loopEndIdx - 1)
                    {
                        const double over = readPosition - (loopEndIdx - 1);
                        readPosition = (loopEndIdx - 1) - over;
                        direction    = -direction;
                        updatePitchRatio();
                    }
                    else if (readPosition < loopStartIdx)
                    {
                        const double under = loopStartIdx - readPosition;
                        readPosition = loopStartIdx + under;
                        direction    = -direction;
                        updatePitchRatio();
                    }
                }
            }

            // Floor toward -infinity so reverse playback (readPosition crossing
            // into negatives) is detected by the lower-bound check below.
            const auto idx = static_cast<int> (std::floor (readPosition));

            // idx must be in [startIdx, endIdx). endIdx is clamped at start() so
            // that idx+1 is always in the underlying sample buffer.
            if (idx < startIdx || idx >= endIdx)
            {
                active = false;
                envelope.reset();
                source.reset();
                break;
            }

            // One LFO evaluation per sample, shared across destinations.
            // lfoMod is in [-depth, +depth].
            float lfoMod = 0.0f;
            if (lfoActive)
            {
                lfoMod    = lfoValue (lfoShape, lfoPhase) * lfoDepth;
                lfoPhase += lfoPhaseInc;
                if (lfoPhase >= 1.0) lfoPhase -= 1.0;
            }

            // Advance the parameter ramps. Gain and pan share a derived
            // equal-power computation, so only pay for the trig when one of
            // them actually moved — for a static voice this is two compares.
            {
                const float gv = smoothedGain.getNextValue();
                const float pv = smoothedPan .getNextValue();
                if (gv != lastSmoothedGain || pv != lastSmoothedPan)
                {
                    lastSmoothedGain = gv;
                    lastSmoothedPan  = pv;
                    baseGainLin      = gv;
                    basePan          = pv;
                    const float ang  = (pv + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
                    gainL = gv * std::cos (ang);
                    gainR = gv * std::sin (ang);
                }
                baseCutoffHz = smoothedCutoff.getNextValue();
            }

            float sL, sR;
            readAt (readPosition, sL, sR);

            // Forward-loop crossfade: as the read nears the loop end, blend in
            // the sample one loop-span earlier (the point it will wrap to) so
            // the seam is inaudible.
            if (loopMode == LoopMode::Forward && loopXfadeSamples > 0)
            {
                const double xfadeStart = static_cast<double> (loopEndIdx - loopXfadeSamples);
                if (readPosition > xfadeStart)
                {
                    const float t = juce::jlimit (0.0f, 1.0f,
                        static_cast<float> ((readPosition - xfadeStart)
                                            / static_cast<double> (loopXfadeSamples)));
                    float hL, hR;
                    readAt (readPosition - static_cast<double> (loopEndIdx - loopStartIdx), hL, hR);
                    sL = sL * (1.0f - t) + hL * t;
                    sR = sR * (1.0f - t) + hR * t;
                }
            }

            // Filter, with cutoff optionally swept by the LFO (±4 octaves at
            // full depth), on top of the aftertouch multiplier.
            if (filterMode != FilterMode::Off)
            {
                // Filter envelope contributes a multiplicative octave shift on
                // top of everything else. Advancing it here (rather than with
                // the amp envelope) keeps it aligned with the sample it affects.
                float envMult = 1.0f;
                if (filterEnvActive)
                    envMult = std::pow (2.0f, filterEnv.getNextSample() * filterEnvOctaves);

                if (lfoActive && lfoDest == LfoDest::Cutoff)
                {
                    const float nyquist = static_cast<float> (hostRate * 0.5);
                    const float swept   = baseCutoffHz * aftertouchCutoffMultiplier * envMult
                                          * std::pow (2.0f, lfoMod * 4.0f);
                    filter.setCutoffFrequency (juce::jlimit (20.0f, nyquist - 1.0f, swept));
                }
                else
                {
                    // Live / smoothed cutoff. setCutoffFrequency recomputes
                    // coefficients, so only call it when the value moved —
                    // a static voice pays one float compare per sample.
                    const float target = baseCutoffHz * aftertouchCutoffMultiplier * envMult;
                    if (target != lastCutoffSet)
                    {
                        lastCutoffSet = target;
                        const float nyquist = static_cast<float> (hostRate * 0.5);
                        filter.setCutoffFrequency (juce::jlimit (20.0f, nyquist - 1.0f, target));
                    }
                }
                sL = filter.processSample (0, sL);
                sR = filter.processSample (1, sR);
            }

            // Character chain: saturation -> sample-rate reduction -> bit depth.
            if (characterActive)
            {
                if (satAmount > 0.0f)
                {
                    sL = applySaturation (sL, satMode, satAmount);
                    sR = applySaturation (sR, satMode, satAmount);
                }
                if (downsampleFactor > 1)
                {
                    if (crushCounter <= 0)
                    {
                        crushHoldL   = sL;
                        crushHoldR   = sR;
                        crushCounter = downsampleFactor;
                    }
                    sL = crushHoldL;
                    sR = crushHoldR;
                    --crushCounter;
                }
                if (bitDepth < 16)
                {
                    sL = applyBitDepthReduction (sL, bitDepth);
                    sR = applyBitDepthReduction (sR, bitDepth);
                }
            }

            // Gains — volume (tremolo) or pan (auto-pan) optionally driven by
            // the LFO. Only one of the two is recomputed per sample.
            float gL = gainL, gR = gainR;
            if (lfoActive && lfoDest == LfoDest::Volume)
            {
                const float vol = juce::jmax (0.0f, 1.0f + lfoMod);
                gL *= vol;
                gR *= vol;
            }
            else if (lfoActive && lfoDest == LfoDest::Pan)
            {
                const float p   = juce::jlimit (-1.0f, 1.0f, basePan + lfoMod);
                const float ang = (p + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
                gL = baseGainLin * std::cos (ang);
                gR = baseGainLin * std::sin (ang);
            }

            // Edge fades. Applied at the two physical window boundaries (by
            // absolute position) so both forward and reverse playback get a
            // click-free entry and exit.
            float fade = 1.0f;
            if (fadeInSamples > 0)
            {
                const int fromStart = idx - startIdx;
                if (fromStart < fadeInSamples)
                    fade *= static_cast<float> (fromStart) / static_cast<float> (fadeInSamples);
            }
            if (fadeOutSamples > 0)
            {
                const int toEnd = endIdx - idx;
                if (toEnd <= fadeOutSamples)
                    fade *= juce::jlimit (0.0f, 1.0f,
                                          static_cast<float> (toEnd) / static_cast<float> (fadeOutSamples));
            }

            const float env    = envelope.getNextSample();
            const float atGain = aftertouchGainMultiplier;
            const float gate   = padGate;

            float outL = sL * env * gL * atGain * gate * fade;
            float outR = sR * env * gR * atGain * gate * fade;

            // Stereo shaping, applied last so it sees the finished signal.
            if (stereoShaping)
            {
                // Mono-below crossover: split off the low band with a one-pole
                // low-pass, sum it to mono, and put it back. Keeps bass and
                // 808s centred and phase-coherent without touching the top.
                if (monoLpCoeff > 0.0f)
                {
                    monoLpStateL += monoLpCoeff * (outL - monoLpStateL);
                    monoLpStateR += monoLpCoeff * (outR - monoLpStateR);

                    const float lowMono = 0.5f * (monoLpStateL + monoLpStateR);
                    outL = (outL - monoLpStateL) + lowMono;
                    outR = (outR - monoLpStateR) + lowMono;
                }

                // M/S width. 0 collapses to mono, 1 is unchanged, 2 doubles
                // the side signal.
                if (std::abs (stereoWidth - 1.0f) > 0.001f)
                {
                    const float mid  = 0.5f * (outL + outR);
                    const float side = 0.5f * (outL - outR) * stereoWidth;
                    outL = mid + side;
                    outR = mid - side;
                }
            }

            output.addSample (0, startSample + i, outL);
            output.addSample (1, startSample + i, outR);

            // Advance the read position. Pitch LFO scales the increment
            // (±1 octave at full depth); pitchRatio already carries base pitch,
            // bend, glide, pitch-env and direction.
            double inc = pitchRatio;
            if (lfoActive && lfoDest == LfoDest::Pitch)
                inc *= std::pow (2.0, static_cast<double> (lfoMod));
            readPosition += inc;

            // Pitch-env and glide decays both feed pitchRatio; recompute once.
            bool pitchChanged = false;
            if (pitchEnvRemainingSamples > 0)
            {
                pitchEnvScale += pitchEnvIncrement;
                --pitchEnvRemainingSamples;
                if (pitchEnvRemainingSamples == 0)
                {
                    pitchEnvScale     = 1.0;
                    pitchEnvIncrement = 0.0;
                }
                pitchChanged = true;
            }
            if (glideRemainingSamples > 0)
            {
                glideSemitones += glideSemitoneStep;
                --glideRemainingSamples;
                if (glideRemainingSamples == 0)
                {
                    glideSemitones    = 0.0;
                    glideSemitoneStep = 0.0;
                    glideScale        = 1.0;
                }
                else
                {
                    glideScale = std::pow (2.0, glideSemitones / 12.0);
                }
                pitchChanged = true;
            }
            if (pitchChanged)
                updatePitchRatio();

            if (! envelope.isActive())
            {
                active = false;
                source.reset();
                break;
            }
        }
    }
}
