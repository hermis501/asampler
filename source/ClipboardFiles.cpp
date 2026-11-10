#include "ClipboardFiles.h"

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <shellapi.h>
 #pragma comment (lib, "shell32.lib")
#endif

namespace asampler
{
    juce::Array<juce::File> getFilesFromClipboard()
    {
        juce::Array<juce::File> files;

       #if JUCE_WINDOWS
        if (OpenClipboard (nullptr))
        {
            if (auto handle = static_cast<HDROP> (GetClipboardData (CF_HDROP)))
            {
                const UINT count = DragQueryFileW (handle, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i)
                {
                    const UINT len = DragQueryFileW (handle, i, nullptr, 0);
                    juce::HeapBlock<wchar_t> buf (len + 1, true);
                    if (DragQueryFileW (handle, i, buf.get(), len + 1) > 0)
                        files.add (juce::File (juce::String (buf.get())));
                }
            }
            CloseClipboard();
        }
       #endif

        if (files.isEmpty())
        {
            // Paths copied as text (a file manager's "copy path", or macOS).
            for (auto line : juce::StringArray::fromLines (juce::SystemClipboard::getTextFromClipboard()))
            {
                line = line.trim().unquoted();
                if (juce::File::isAbsolutePath (line) && juce::File (line).existsAsFile())
                    files.add (juce::File (line));
            }
        }

        return files;
    }
}
