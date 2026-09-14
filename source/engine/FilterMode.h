#pragma once

namespace asampler
{
    // Per-pad filter mode. Order matters: AudioParameterChoice serializes the
    // value as an integer index, so any reordering would silently corrupt
    // saved state. Append-only.
    enum class FilterMode : int
    {
        Off      = 0,
        LowPass  = 1,
        HighPass = 2,
        BandPass = 3
    };
}
