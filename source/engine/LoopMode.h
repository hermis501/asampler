#pragma once

namespace asampler
{
    // Per-pad loop mode. Order matters: AudioParameterChoice serializes the
    // value as an integer index, so any reordering would silently corrupt
    // saved state. Append-only.
    enum class LoopMode : int
    {
        Off      = 0,   // play through the [start, end) window once
        Forward  = 1,   // when readPosition crosses loopEnd, wrap to loopStart
        PingPong = 2    // bounce direction at loopStart and loopEnd
    };
}
