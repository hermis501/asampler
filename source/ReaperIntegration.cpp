#include "ReaperIntegration.h"

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wshadow-field-in-constructor",
                                     "-Wnon-virtual-dtor",
                                     "-Wpragma-pack",
                                     "-Wzero-as-null-pointer-constant",
                                     "-Wunused-parameter")
JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4100)

#include <pluginterfaces/base/ftypes.h>
#include <pluginterfaces/base/funknown.h>

namespace asampler::reaper
{
    using namespace Steinberg;

    // From Cockos' reaper_vst3_interfaces.h (REAPER 5.02+): the host object
    // REAPER passes to setIHostApplication implements this.
    class IReaperHostApplication : public FUnknown
    {
    public:
        virtual void* PLUGIN_API getReaperApi (CStringA funcname) = 0;
        virtual void* PLUGIN_API getReaperParent (uint32 w) = 0;
        virtual void* PLUGIN_API reaperExtended (uint32 call, void* parm1, void* parm2, void* parm3) = 0;
        static const FUID iid;
    };

    DECLARE_CLASS_IID (IReaperHostApplication, 0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C)
    DEF_CLASS_IID (IReaperHostApplication)
}

JUCE_END_IGNORE_WARNINGS_MSVC
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace asampler
{
    namespace
    {
        class ReaperVst3Extensions final : public juce::VST3ClientExtensions
        {
        public:
            explicit ReaperVst3Extensions (ReaperApi& a) : api (a) {}

            void setIHostApplication (Steinberg::FUnknown* host) override
            {
                api = {};
                if (host == nullptr)
                    return;

                void* obj = nullptr;
                if (host->queryInterface (reaper::IReaperHostApplication::iid, &obj) != Steinberg::kResultOk
                    || obj == nullptr)
                    return;

                auto* reaperHost = static_cast<reaper::IReaperHostApplication*> (obj);
                auto get = [reaperHost] (const char* name) { return reaperHost->getReaperApi (name); };

                api.csurfOnRecord           = reinterpret_cast<void (*) ()> (get ("CSurf_OnRecord"));
                api.countSelectedMediaItems = reinterpret_cast<int (*) (void*)> (get ("CountSelectedMediaItems"));
                api.getSelectedMediaItem    = reinterpret_cast<void* (*) (void*, int)> (get ("GetSelectedMediaItem"));
                api.mainOnCommand           = reinterpret_cast<void (*) (int, int)> (get ("Main_OnCommand"));
                api.getActiveTake           = reinterpret_cast<void* (*) (void*)> (get ("GetActiveTake"));
                api.getMediaItemTakeSource  = reinterpret_cast<void* (*) (void*)> (get ("GetMediaItemTake_Source"));
                api.getMediaSourceFileName  = reinterpret_cast<void (*) (void*, char*, int)> (get ("GetMediaSourceFileName"));
                api.undoDoUndo2             = reinterpret_cast<int (*) (void*)> (get ("Undo_DoUndo2"));
                api.getProjectStateChangeCount = reinterpret_cast<int (*) (void*)> (get ("GetProjectStateChangeCount"));

                reaperHost->release();   // queryInterface added a reference
            }

        private:
            ReaperApi& api;
        };
    }

    std::unique_ptr<juce::VST3ClientExtensions> makeReaperVst3Extensions (ReaperApi& api)
    {
        return std::make_unique<ReaperVst3Extensions> (api);
    }
}
