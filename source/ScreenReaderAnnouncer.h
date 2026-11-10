#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace asampler
{
    // Speaks a message through the user's screen reader, even when keyboard
    // focus did not move.
    //
    // JUCE's AccessibilityHandler::postAnnouncement does not do this on
    // Windows: it speaks through a separate SAPI voice, so the announcement
    // comes out in a different voice and rate from NVDA/JAWS, ignores the
    // screen reader's mute and speech-history keys, and talks over it. This
    // class instead raises a UI Automation *notification event* — the same
    // mechanism OSARA uses inside REAPER — which the screen reader itself
    // speaks in the user's own voice and keeps in its speech history.
    //
    // How: a hidden zero-size child window of the editor answers WM_GETOBJECT
    // with a minimal UIA provider, and notifications are raised from that
    // provider. UiaRaiseNotificationEvent only exists on Windows 10 1709 and
    // later, so it is looked up at run time; where it is missing the call falls
    // back to JUCE's announcement rather than the plugin failing to load.
    //
    // On macOS JUCE's announcement already goes through VoiceOver
    // (NSAccessibilityAnnouncementRequestedNotification), so it is used as-is.
    class ScreenReaderAnnouncer
    {
    public:
        // `owner` is the component whose native window hosts the notification
        // source — normally the plugin editor. It must outlive this object.
        explicit ScreenReaderAnnouncer (juce::Component& owner);
        ~ScreenReaderAnnouncer();

        enum class Route
        {
            UiaNotification,   // raised as a UIA notification event (Windows)
            JuceAnnouncement,  // handed to AccessibilityHandler::postAnnouncement
            Dropped            // nothing to say, or no window to say it from
        };

        // `interrupt` = true for failures and warnings: the screen reader stops
        // what it is saying. Routine confirmations queue behind current speech
        // so they never talk over the user.
        Route announce (const juce::String& message, bool interrupt);

        // Create the notification source now rather than at the first
        // announcement. Call once the owner is on screen: a screen reader
        // needs a moment to discover a new window, and a message raised from
        // one it has not seen yet can be lost. Safe to call repeatedly.
        void prepare();

    private:
        juce::Component& owner;

        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE (ScreenReaderAnnouncer)
    };
}
