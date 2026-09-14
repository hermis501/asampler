#include "VocalSynth.h"

#if JUCE_WINDOWS

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <sapi.h>

#include <string>

// Auto-link the Speech API + COM. Keeps the plugin and the test binary (which
// pulls this TU out of ASampler_SharedCode) linking without extra CMake wiring.
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "ole32.lib")

namespace asampler
{
    namespace
    {
        template <typename T>
        void safeRelease (T*& p) noexcept
        {
            if (p != nullptr) { p->Release(); p = nullptr; }
        }

        // BMP-only widen (speech text and file paths are practically always in
        // the basic plane); avoids depending on JUCE's UTF-16 pointer lifetime.
        std::wstring toWide (const juce::String& s)
        {
            std::wstring w;
            w.reserve (static_cast<std::size_t> (s.length()));
            for (auto p = s.getCharPointer(); ! p.isEmpty();)
                w.push_back (static_cast<wchar_t> (p.getAndAdvance()));
            return w;
        }
    }

    VocalSynthResult synthesizeSpeechToWav (const juce::String& text,
                                            const juce::File&   outWav,
                                            int                 sampleRate)
    {
        VocalSynthResult result;

        if (text.trim().isEmpty())
        {
            result.error = "No text to speak";
            return result;
        }

        outWav.getParentDirectory().createDirectory();
        outWav.deleteFile();

        // Initialise COM on this thread. S_OK / S_FALSE both mean "we own an
        // init and must balance it"; RPC_E_CHANGED_MODE means the apartment was
        // already initialised in another mode — proceed but do NOT uninitialise.
        const HRESULT coHr           = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        const bool    ownsComInit    = SUCCEEDED (coHr);

        ISpVoice*  voice   = nullptr;
        ISpStream* stream  = nullptr;
        bool       spokeOk = false;
        HRESULT    hr      = S_OK;

        hr = CoCreateInstance (__uuidof (SpVoice), nullptr, CLSCTX_ALL,
                               __uuidof (ISpVoice), reinterpret_cast<void**> (&voice));

        if (SUCCEEDED (hr))
            hr = CoCreateInstance (__uuidof (SpStream), nullptr, CLSCTX_ALL,
                                   __uuidof (ISpStream), reinterpret_cast<void**> (&stream));

        if (SUCCEEDED (hr))
        {
            WAVEFORMATEX wfx {};
            wfx.wFormatTag      = WAVE_FORMAT_PCM;
            wfx.nChannels       = 1;
            wfx.nSamplesPerSec  = static_cast<DWORD> (juce::jlimit (8000, 48000, sampleRate));
            wfx.wBitsPerSample  = 16;
            wfx.nBlockAlign     = static_cast<WORD>  (wfx.nChannels * wfx.wBitsPerSample / 8);
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            wfx.cbSize          = 0;

            const std::wstring wpath = toWide (outWav.getFullPathName());
            hr = stream->BindToFile (wpath.c_str(), SPFM_CREATE_ALWAYS,
                                     &SPDFID_WaveFormatEx, &wfx, SPFEI_ALL_EVENTS);
        }

        if (SUCCEEDED (hr))
            hr = voice->SetOutput (stream, TRUE);

        if (SUCCEEDED (hr))
        {
            const std::wstring wtext = toWide (text);
            // Synchronous (no SPF_ASYNC). SPF_IS_NOT_XML so punctuation like '<'
            // in the user's text isn't parsed as SSML markup.
            hr = voice->Speak (wtext.c_str(), SPF_DEFAULT | SPF_IS_NOT_XML, nullptr);
            spokeOk = SUCCEEDED (hr);
        }

        if (stream != nullptr)
            stream->Close();   // flush audio + finalise the WAV header

        safeRelease (stream);
        safeRelease (voice);

        if (ownsComInit)
            CoUninitialize();

        if (! spokeOk)
        {
            outWav.deleteFile();
            result.error = "Speech engine failed (no voice installed?) hr=0x"
                         + juce::String::toHexString (static_cast<int> (hr));
            return result;
        }

        if (! outWav.existsAsFile() || outWav.getSize() < 64)
        {
            result.error = "Speech produced no audio";
            return result;
        }

        result.ok = true;
        return result;
    }
}

#else  // ---------------------------------------------------------------------

namespace asampler
{
    VocalSynthResult synthesizeSpeechToWav (const juce::String&, const juce::File&, int)
    {
        return { false, "SAPI5 speech synthesis is Windows-only" };
    }
}

#endif
