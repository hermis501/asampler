#if defined (_WIN32)
 // Windows headers go first, trimmed, so their macros (min/max, Rectangle...)
 // are in place before JUCE's headers rather than ambushing them afterwards.
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <ole2.h>
 #include <UIAutomation.h>
 #pragma comment (lib, "oleaut32.lib")
#endif

#include "ScreenReaderAnnouncer.h"

#include <atomic>

namespace asampler
{
   #if JUCE_WINDOWS
    namespace
    {
        // Every UIA entry point is resolved at run time. UiaRaiseNotificationEvent
        // is the reason: it only exists on Windows 10 1709+, and a static import
        // of it would stop the whole plugin DLL loading on anything older.
        struct UiaApi
        {
            using ReturnRawProviderFn = LRESULT (WINAPI*) (HWND, WPARAM, LPARAM, IRawElementProviderSimple*);
            using HostFromHwndFn      = HRESULT (WINAPI*) (HWND, IRawElementProviderSimple**);
            using RaiseNotificationFn = HRESULT (WINAPI*) (IRawElementProviderSimple*, NotificationKind,
                                                           NotificationProcessing, BSTR, BSTR);
            using DisconnectFn        = HRESULT (WINAPI*) (IRawElementProviderSimple*);

            ReturnRawProviderFn returnRawProvider = nullptr;
            HostFromHwndFn      hostFromHwnd      = nullptr;
            RaiseNotificationFn raiseNotification = nullptr;
            DisconnectFn        disconnect        = nullptr;

            static const UiaApi& get()
            {
                static const UiaApi api;
                return api;
            }

            bool canNotify() const noexcept
            {
                return returnRawProvider != nullptr && hostFromHwnd != nullptr
                    && raiseNotification != nullptr;
            }

        private:
            UiaApi()
            {
                // Loaded once and deliberately never freed: the function
                // pointers must stay valid for the life of the process.
                if (auto* dll = LoadLibraryW (L"UIAutomationCore.dll"))
                {
                    returnRawProvider = reinterpret_cast<ReturnRawProviderFn> (GetProcAddress (dll, "UiaReturnRawElementProvider"));
                    hostFromHwnd      = reinterpret_cast<HostFromHwndFn>      (GetProcAddress (dll, "UiaHostProviderFromHwnd"));
                    raiseNotification = reinterpret_cast<RaiseNotificationFn> (GetProcAddress (dll, "UiaRaiseNotificationEvent"));
                    disconnect        = reinterpret_cast<DisconnectFn>        (GetProcAddress (dll, "UiaDisconnectProvider"));
                }
            }
        };

        // The smallest provider UIA will accept as a notification source. It
        // reports itself as neither a control nor content element, so screen
        // readers never land on it while navigating the editor.
        class NotificationProvider final : public IRawElementProviderSimple
        {
        public:
            explicit NotificationProvider (HWND w) : hwnd (w) {}

            // IUnknown
            HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** out) override
            {
                if (out == nullptr)
                    return E_POINTER;

                if (riid == __uuidof (IUnknown) || riid == __uuidof (IRawElementProviderSimple))
                {
                    *out = static_cast<IRawElementProviderSimple*> (this);
                    AddRef();
                    return S_OK;
                }

                *out = nullptr;
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override  { return ++refCount; }

            ULONG STDMETHODCALLTYPE Release() override
            {
                const auto remaining = --refCount;
                if (remaining == 0)
                    delete this;
                return remaining;
            }

            // IRawElementProviderSimple
            HRESULT STDMETHODCALLTYPE get_ProviderOptions (ProviderOptions* out) override
            {
                if (out == nullptr) return E_POINTER;
                // COM threading: UIA marshals calls onto the window's thread
                // (the message thread), so nothing here needs locking.
                *out = static_cast<ProviderOptions> (ProviderOptions_ServerSideProvider
                                                     | ProviderOptions_UseComThreading);
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetPatternProvider (PATTERNID, IUnknown** out) override
            {
                if (out == nullptr) return E_POINTER;
                *out = nullptr;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetPropertyValue (PROPERTYID id, VARIANT* out) override
            {
                if (out == nullptr) return E_POINTER;
                VariantInit (out);

                switch (id)
                {
                    case UIA_ControlTypePropertyId:
                        out->vt   = VT_I4;
                        out->lVal = UIA_CustomControlTypeId;
                        break;

                    case UIA_IsControlElementPropertyId:
                    case UIA_IsContentElementPropertyId:
                        out->vt      = VT_BOOL;
                        out->boolVal = VARIANT_FALSE;
                        break;

                    case UIA_ProviderDescriptionPropertyId:
                        out->vt      = VT_BSTR;
                        out->bstrVal = SysAllocString (L"ASampler announcements");
                        break;

                    default:
                        break;   // VT_EMPTY: let UIA use the host window's value
                }
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE get_HostRawElementProvider (IRawElementProviderSimple** out) override
            {
                if (out == nullptr) return E_POINTER;
                *out = nullptr;
                if (auto fn = UiaApi::get().hostFromHwnd)
                    return fn (hwnd, out);
                return E_FAIL;
            }

        private:
            HWND hwnd;
            std::atomic<ULONG> refCount { 1 };
        };

        // One window class per loaded copy of the DLL, registered while any
        // announcer exists and unregistered with the last one. A class left
        // registered after the DLL unloads would keep a dangling WndProc.
        constexpr const wchar_t* kWindowClass = L"ASamplerUiaAnnouncer";
        int classUsers = 0;   // message thread only

        LRESULT CALLBACK announcerWndProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            if (msg == WM_GETOBJECT && static_cast<long> (lParam) == static_cast<long> (UiaRootObjectId))
            {
                auto* provider = reinterpret_cast<NotificationProvider*> (
                    GetWindowLongPtrW (hwnd, GWLP_USERDATA));

                if (provider != nullptr)
                    if (auto fn = UiaApi::get().returnRawProvider)
                        return fn (hwnd, wParam, lParam, provider);
            }
            return DefWindowProcW (hwnd, msg, wParam, lParam);
        }

        HINSTANCE moduleInstance()
        {
            return static_cast<HINSTANCE> (juce::Process::getCurrentModuleInstanceHandle());
        }

        bool acquireWindowClass()
        {
            if (classUsers++ > 0)
                return true;

            WNDCLASSEXW wc {};
            wc.cbSize        = sizeof (wc);
            wc.lpfnWndProc   = announcerWndProc;
            wc.hInstance     = moduleInstance();
            wc.lpszClassName = kWindowClass;

            if (RegisterClassExW (&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
                return true;

            --classUsers;
            return false;
        }

        void releaseWindowClass()
        {
            if (classUsers > 0 && --classUsers == 0)
                UnregisterClassW (kWindowClass, moduleInstance());
        }

        BSTR toBstr (const juce::String& s)
        {
            return SysAllocString (s.toWideCharPointer());
        }
    }

    struct ScreenReaderAnnouncer::Impl
    {
        HWND                  parent   = nullptr;
        HWND                  window   = nullptr;
        NotificationProvider* provider = nullptr;
        bool                  haveClass = false;

        ~Impl() { destroyWindow(); if (haveClass) releaseWindowClass(); }

        // (Re)creates the notification window under `newParent`. The editor's
        // native window can be recreated by the host (e.g. docking an FX
        // window in REAPER), so this is checked on every announcement.
        bool ensureWindow (HWND newParent)
        {
            if (window != nullptr && parent == newParent && IsWindow (window))
                return true;

            destroyWindow();

            if (newParent == nullptr)
                return false;

            if (! haveClass)
                haveClass = acquireWindowClass();
            if (! haveClass)
                return false;

            // Visible but zero-sized: some screen readers ignore events from
            // hidden windows, and a 0x0 child cannot be seen or clicked.
            window = CreateWindowExW (0, kWindowClass, L"", WS_CHILD | WS_VISIBLE,
                                      0, 0, 0, 0, newParent, nullptr, moduleInstance(), nullptr);
            if (window == nullptr)
                return false;

            provider = new NotificationProvider (window);
            SetWindowLongPtrW (window, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (provider));
            parent = newParent;
            return true;
        }

        void destroyWindow()
        {
            if (window != nullptr)
            {
                SetWindowLongPtrW (window, GWLP_USERDATA, 0);

                // Tell UIA the provider is gone before the window is, so no
                // client keeps calling into an object about to be freed.
                if (auto fn = UiaApi::get().returnRawProvider)
                    fn (window, 0, 0, nullptr);
                if (provider != nullptr)
                    if (auto fn = UiaApi::get().disconnect)
                        fn (provider);

                DestroyWindow (window);
                window = nullptr;
            }

            if (provider != nullptr)
            {
                provider->Release();
                provider = nullptr;
            }
            parent = nullptr;
        }

        bool raise (const juce::String& message, bool interrupt)
        {
            const auto& api = UiaApi::get();
            if (! api.canNotify() || provider == nullptr)
                return false;

            // ImportantMostRecent: the screen reader drops what it was saying
            // and speaks this now. All: queued in order behind current speech.
            const auto processing = interrupt ? NotificationProcessing_ImportantMostRecent
                                              : NotificationProcessing_All;

            BSTR text     = toBstr (message);
            BSTR activity = toBstr (interrupt ? "ASampler.Alert" : "ASampler.Status");
            const HRESULT hr = api.raiseNotification (provider, NotificationKind_Other,
                                                      processing, text, activity);
            SysFreeString (text);
            SysFreeString (activity);
            return SUCCEEDED (hr);
        }
    };
   #else
    struct ScreenReaderAnnouncer::Impl {};
   #endif

    ScreenReaderAnnouncer::ScreenReaderAnnouncer (juce::Component& c)
        : owner (c), impl (std::make_unique<Impl>())
    {
    }

    ScreenReaderAnnouncer::~ScreenReaderAnnouncer() = default;

    void ScreenReaderAnnouncer::prepare()
    {
       #if JUCE_WINDOWS
        impl->ensureWindow (static_cast<HWND> (owner.getWindowHandle()));
       #endif
    }

    ScreenReaderAnnouncer::Route ScreenReaderAnnouncer::announce (const juce::String& message,
                                                                  bool interrupt)
    {
        if (message.trim().isEmpty())
            return Route::Dropped;

       #if JUCE_WINDOWS
        if (impl->ensureWindow (static_cast<HWND> (owner.getWindowHandle()))
            && impl->raise (message, interrupt))
            return Route::UiaNotification;
       #endif

        if (auto* handler = owner.getAccessibilityHandler())
        {
            handler->postAnnouncement (
                message,
                interrupt ? juce::AccessibilityHandler::AnnouncementPriority::high
                          : juce::AccessibilityHandler::AnnouncementPriority::medium);
            return Route::JuceAnnouncement;
        }

        return Route::Dropped;
    }
}
