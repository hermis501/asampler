#pragma once

#include <cmath>

namespace asampler
{
    // Maps MIDI velocity (0..1) to the gain multiplier applied at voice trigger.
    // Order matters: AudioParameterChoice stores the value as an integer index,
    // so any reordering here would silently corrupt saved state. Append-only.
    enum class VelocityCurve : int
    {
        Fixed  = 0,   // ignore velocity, always full level — drums where every hit matters
        Soft   = 1,   // vel^2 — more dynamic range, soft hits very quiet
        Linear = 2,   // identity — default
        Hard   = 3    // sqrt(vel) — compressed, soft hits boosted
    };

    inline float applyVelocityCurve (float velocity, VelocityCurve curve) noexcept
    {
        velocity = velocity < 0.0f ? 0.0f : (velocity > 1.0f ? 1.0f : velocity);

        switch (curve)
        {
            case VelocityCurve::Fixed:  return 1.0f;
            case VelocityCurve::Soft:   return velocity * velocity;
            case VelocityCurve::Linear: return velocity;
            case VelocityCurve::Hard:   return std::sqrt (velocity);
        }
        return velocity;
    }
}
