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

            ~ReaperVst3Extensions() override
            {
                api.getParent = nullptr;
                if (reaperHost != nullptr)
                    reaperHost->release();
            }

            void setIHostApplication (Steinberg::FUnknown* host) override
            {
                api = {};
                if (reaperHost != nullptr)
                {
                    reaperHost->release();
                    reaperHost = nullptr;
                }
                if (host == nullptr)
                    return;

                void* obj = nullptr;
                if (host->queryInterface (reaper::IReaperHostApplication::iid, &obj) != Steinberg::kResultOk
                    || obj == nullptr)
                    return;

                auto* rh = static_cast<reaper::IReaperHostApplication*> (obj);
                auto get = [rh] (const char* name) { return rh->getReaperApi (name); };

                api.csurfOnRecord           = reinterpret_cast<void (*) ()> (get ("CSurf_OnRecord"));
                api.countSelectedMediaItems = reinterpret_cast<int (*) (void*)> (get ("CountSelectedMediaItems"));
                api.getSelectedMediaItem    = reinterpret_cast<void* (*) (void*, int)> (get ("GetSelectedMediaItem"));
                api.mainOnCommand           = reinterpret_cast<void (*) (int, int)> (get ("Main_OnCommand"));
                api.getActiveTake           = reinterpret_cast<void* (*) (void*)> (get ("GetActiveTake"));
                api.getMediaItemTakeSource  = reinterpret_cast<void* (*) (void*)> (get ("GetMediaItemTake_Source"));
                api.getMediaSourceFileName  = reinterpret_cast<void (*) (void*, char*, int)> (get ("GetMediaSourceFileName"));
                api.undoDoUndo2             = reinterpret_cast<int (*) (void*)> (get ("Undo_DoUndo2"));
                api.getProjectStateChangeCount = reinterpret_cast<int (*) (void*)> (get ("GetProjectStateChangeCount"));
                api.undoOnStateChangeEx2    = reinterpret_cast<void (*) (void*, const char*, int, int)> (get ("Undo_OnStateChangeEx2"));
                api.getExtState             = reinterpret_cast<const char* (*) (const char*, const char*)> (get ("GetExtState"));
                api.deleteExtState          = reinterpret_cast<void (*) (const char*, const char*, bool)> (get ("DeleteExtState"));
                api.getTrackGuid            = reinterpret_cast<void* (*) (void*)> (get ("GetTrackGUID"));
                api.guidToString            = reinterpret_cast<void (*) (const void*, char*)> (get ("guidToString"));

                api.countMediaItems         = reinterpret_cast<int (*) (void*)> (get ("CountMediaItems"));
                api.getMediaItem            = reinterpret_cast<void* (*) (void*, int)> (get ("GetMediaItem"));
                api.getSetMediaItemInfoString = reinterpret_cast<bool (*) (void*, const char*, char*, bool)> (get ("GetSetMediaItemInfo_String"));
                api.getMediaItemInfoValue   = reinterpret_cast<double (*) (void*, const char*)> (get ("GetMediaItemInfo_Value"));
                api.setMediaItemSelected    = reinterpret_cast<void (*) (void*, bool)> (get ("SetMediaItemSelected"));
                api.selectAllMediaItems     = reinterpret_cast<void (*) (void*, bool)> (get ("SelectAllMediaItems"));
                api.updateArrange           = reinterpret_cast<void (*) ()> (get ("UpdateArrange"));

                api.getTouchedOrFocusedFX   = reinterpret_cast<bool (*) (int, int*, int*, int*, int*, int*)> (get ("GetTouchedOrFocusedFX"));
                api.getTrack                = reinterpret_cast<void* (*) (void*, int)> (get ("GetTrack"));
                api.getMasterTrack          = reinterpret_cast<void* (*) (void*)> (get ("GetMasterTrack"));
                api.trackFXGetParamName     = reinterpret_cast<bool (*) (void*, int, int, char*, int)> (get ("TrackFX_GetParamName"));
                api.getFXEnvelope           = reinterpret_cast<void* (*) (void*, int, int, bool)> (get ("GetFXEnvelope"));
                api.getSetEnvelopeInfoString = reinterpret_cast<bool (*) (void*, const char*, char*, bool)> (get ("GetSetEnvelopeInfo_String"));
                api.trackListAdjustWindows  = reinterpret_cast<void (*) (bool)> (get ("TrackList_AdjustWindows"));

                // Kept (with the reference queryInterface added) so the parent
                // track can be asked for later; released in the destructor.
                reaperHost = rh;
                api.getParent = [rh] (int which) { return rh->getReaperParent ((Steinberg::uint32) which); };
            }

        private:
            ReaperApi& api;
            reaper::IReaperHostApplication* reaperHost = nullptr;
        };
    }

    std::unique_ptr<juce::VST3ClientExtensions> makeReaperVst3Extensions (ReaperApi& api)
    {
        return std::make_unique<ReaperVst3Extensions> (api);
    }
}
