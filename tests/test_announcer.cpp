// End-to-end check that ScreenReaderAnnouncer reaches a screen reader.
//
// A UIA client on a separate thread listens for notification events from a
// real on-desktop component's window tree, then checks that an announcement
// arrives with its text and interrupt priority intact. That is the property that matters: an announcement that
// fires but never reaches a listener is silence to the user.

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <ole2.h>
 #include <UIAutomation.h>
 #pragma comment (lib, "ole32.lib")
 #pragma comment (lib, "oleaut32.lib")
#endif

#include <catch2/catch_test_macros.hpp>

#include "ScreenReaderAnnouncer.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using asampler::ScreenReaderAnnouncer;

TEST_CASE ("An empty announcement is dropped rather than spoken",
           "[accessibility][announce]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::Component c;
    ScreenReaderAnnouncer announcer (c);

    REQUIRE (announcer.announce ({}, false)     == ScreenReaderAnnouncer::Route::Dropped);
    REQUIRE (announcer.announce ("   ", true)   == ScreenReaderAnnouncer::Route::Dropped);
}

#if JUCE_WINDOWS
namespace
{
    class Listener final : public IUIAutomationNotificationEventHandler
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** out) override
        {
            if (riid == __uuidof (IUnknown) || riid == __uuidof (IUIAutomationNotificationEventHandler))
            {
                *out = static_cast<IUIAutomationNotificationEventHandler*> (this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override  { return ++refs; }
        ULONG STDMETHODCALLTYPE Release() override { return --refs; }   // stack-owned

        HRESULT STDMETHODCALLTYPE HandleNotificationEvent (IUIAutomationElement*,
                                                           NotificationKind,
                                                           NotificationProcessing processing,
                                                           BSTR displayString,
                                                           BSTR) override
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (displayString != nullptr)
                received.add (juce::String (displayString));
            lastProcessing = processing;
            return S_OK;
        }

        bool hasReceived (const juce::String& text)
        {
            std::lock_guard<std::mutex> lock (mutex);
            return received.contains (text);
        }

        NotificationProcessing processingSeen()
        {
            std::lock_guard<std::mutex> lock (mutex);
            return lastProcessing;
        }

    private:
        std::atomic<ULONG> refs { 1 };
        std::mutex mutex;
        juce::StringArray received;
        NotificationProcessing lastProcessing = NotificationProcessing_All;
    };

    void pumpMessagesFor (std::chrono::milliseconds duration)
    {
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until)
        {
            MSG msg;
            while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage (&msg);
                DispatchMessageW (&msg);
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    }
}

TEST_CASE ("Announcements reach a UIA notification listener on the desktop",
           "[accessibility][announce]")
{
    juce::ScopedJuceInitialiser_GUI gui;

    juce::Component editor;
    editor.setBounds (0, 0, 200, 100);
    // Visible, like a real editor: a desktop-wide listener (NVDA's setup)
    // does not receive notifications from inside a hidden window.
    editor.setVisible (true);
    editor.addToDesktop (0);
    REQUIRE (editor.getWindowHandle() != nullptr);

    ScreenReaderAnnouncer announcer (editor);
    const auto editorHwnd = static_cast<HWND> (editor.getWindowHandle());
    announcer.prepare();   // as the editor does once it is on screen

    // Raise once before any client exists. Here the client and the provider
    // share a process, and until this process has raised an event a client
    // registration in it fails with E_FAIL. A screen reader runs in its own
    // process and is unaffected.
    announcer.announce ("warm-up", false);

    Listener listener;
    std::atomic<int>  clientState { 0 };   // 0 starting, 1 listening, -1 unavailable
    std::atomic<bool> done { false };
    std::atomic<long> clientError { 0 };   // first failing HRESULT, for the skip message

    // UIA clients must not run on the thread that owns the UI they observe,
    // or a provider call can deadlock waiting for that thread — so the
    // listener lives on its own MTA thread while this one pumps messages.
    std::thread client ([&]
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);

        IUIAutomation5*       automation = nullptr;
        IUIAutomationElement* root       = nullptr;

        HRESULT hr = CoCreateInstance (__uuidof (CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                       __uuidof (IUIAutomation5),
                                       reinterpret_cast<void**> (&automation));
        // Listen on the editor window's subtree; the notification window is a
        // child of it. NVDA listens on the desktop root instead, but a root
        // registration made from inside this short-lived process fails
        // intermittently with E_FAIL, so the narrower scope keeps this stable.
        if (SUCCEEDED (hr)) hr = automation->ElementFromHandle (editorHwnd, &root);
        if (SUCCEEDED (hr)) hr = automation->AddNotificationEventHandler (root, TreeScope_Subtree,
                                                                          nullptr, &listener);
        if (SUCCEEDED (hr))
        {
            clientState = 1;
            while (! done)
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
            automation->RemoveNotificationEventHandler (root, &listener);
        }
        else
        {
            clientError = hr;
            clientState = -1;
        }

        if (root != nullptr)       root->Release();
        if (automation != nullptr) automation->Release();
        CoUninitialize();
    });

    for (int i = 0; i < 1500 && clientState == 0; ++i)
        pumpMessagesFor (std::chrono::milliseconds (10));

    if (clientState != 1)
    {
        done = true;
        client.join();
        SKIP ("UI Automation client unavailable on this machine (HRESULT 0x"
              << juce::String::toHexString ((int) clientError.load()) << ")");
    }

    // A client's registration takes a moment to propagate to providers, so
    // repeat the announcement until it lands (or give up after ~5 s).
    const juce::String text = "ASampler test announcement";
    ScreenReaderAnnouncer::Route route = ScreenReaderAnnouncer::Route::Dropped;
    bool heard = false;
    for (int attempt = 0; attempt < 25 && ! heard; ++attempt)
    {
        route = announcer.announce (text + " " + juce::String (attempt), true);
        pumpMessagesFor (std::chrono::milliseconds (200));
        for (int k = 0; k <= attempt && ! heard; ++k)
            heard = listener.hasReceived (text + " " + juce::String (k));
    }

    done = true;
    client.join();

    REQUIRE (route == ScreenReaderAnnouncer::Route::UiaNotification);
    REQUIRE (heard);
    // interrupt = true must ask the screen reader to cut in, not queue.
    REQUIRE (listener.processingSeen() == NotificationProcessing_ImportantMostRecent);
}
#endif
