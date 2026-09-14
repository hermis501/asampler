#pragma once

#include <juce_core/juce_core.h>

namespace asampler
{
    // Result of a text-to-speech render. `ok == false` carries a human-readable
    // reason suitable for a screen-reader status message.
    struct VocalSynthResult
    {
        bool         ok = false;
        juce::String error;
    };

    // Render `text` to a 16-bit PCM mono WAV at `outWav` using the Windows
    // Speech API (SAPI5). Runs synchronously — intended for the message thread
    // in response to an explicit user action, never the audio thread.
    //
    // On non-Windows builds this is a stub that always fails (SAPI5 is a Windows
    // COM API). The project is Windows-only today; the guard just keeps the file
    // portable if that ever changes.
    VocalSynthResult synthesizeSpeechToWav (const juce::String& text,
                                            const juce::File&   outWav,
                                            int                 sampleRate = 22050);
}
