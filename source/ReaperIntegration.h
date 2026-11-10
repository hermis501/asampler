#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>

namespace asampler
{
    // The handful of REAPER API functions ASampler uses, as plain function
    // pointers. REAPER hands them out through the IReaperHostApplication
    // interface on its VST3 host object; in any other host every pointer stays
    // null and the features that need them say so instead of failing quietly.
    //
    // A table of pointers (rather than calling the interface directly) also
    // lets the tests stand in for REAPER.
    struct ReaperApi
    {
        void   (*csurfOnRecord) ()                                         = nullptr;
        int    (*countSelectedMediaItems) (void* project)                  = nullptr;
        void*  (*getSelectedMediaItem) (void* project, int index)          = nullptr;
        void   (*mainOnCommand) (int command, int flag)                    = nullptr;
        void*  (*getActiveTake) (void* item)                               = nullptr;
        void*  (*getMediaItemTakeSource) (void* take)                      = nullptr;
        void   (*getMediaSourceFileName) (void* source, char* buf, int size) = nullptr;
        int    (*undoDoUndo2) (void* project)                              = nullptr;
        int    (*getProjectStateChangeCount) (void* project)               = nullptr;

        bool canRecord() const noexcept { return csurfOnRecord != nullptr; }

        bool canGrabItems() const noexcept
        {
            return countSelectedMediaItems != nullptr && getSelectedMediaItem != nullptr
                && mainOnCommand != nullptr && getActiveTake != nullptr
                && getMediaItemTakeSource != nullptr && getMediaSourceFileName != nullptr
                && undoDoUndo2 != nullptr && getProjectStateChangeCount != nullptr;
        }
    };

    // VST3 hook that fills a ReaperApi when the host turns out to be REAPER.
    std::unique_ptr<juce::VST3ClientExtensions> makeReaperVst3Extensions (ReaperApi& api);
}
