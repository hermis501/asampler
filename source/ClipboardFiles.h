#pragma once

#include <juce_core/juce_core.h>

namespace asampler
{
    // Files on the system clipboard: what Explorer's Copy puts there (CF_HDROP)
    // on Windows, else text holding one existing file path per line. Empty
    // when the clipboard holds no files.
    juce::Array<juce::File> getFilesFromClipboard();
}
