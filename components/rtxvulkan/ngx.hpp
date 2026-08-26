#pragma once

#include <string>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>

#include <components/rtx/error.hpp>
#include <components/rtx/reconstruction.hpp>

#include "dlss.hpp"

namespace Rtx
{
    /// NGX's own name for a result code, with the code after it.
    ///
    /// **Asked rather than switched on.** The SDK defines several dozen codes as a bitfield —
    /// success is `0x1` and every failure carries `0xBAD00000` with the reason in its low bits — and
    /// it names them all, including ones this build has not been told about.
    inline std::string describeNgxResult(NVSDK_NGX_Result result)
    {
        // `wchar_t` is 32 bits here, not the 16 it is on Windows, which is what truncates every one
        // of these names to a single character if the declaration is copied from the documentation.
        const wchar_t* wide = GetNGXResultAsString(result);
        std::string text;
        for (const wchar_t* at = wide; at != nullptr && *at != L'\0'; ++at)
            text += static_cast<char>(*at);

        return text;
    }

    /// The quality level an upscale setting is, as NGX numbers them.
    ///
    /// **`Off` is refused rather than answered.** It is the absence of an upscaler and not a mode of
    /// one, so there is no quality level it names; the renderer answers it by building no feature at
    /// all, and anything that got here holding it has already decided to build one. Grouping it with
    /// `Performance` for the sake of a total switch made a contradiction into the fastest, softest
    /// mode this renderer has — quietly, and on the path a frame budget is measured against.
    ///
    /// Thrown rather than asserted because it is a cold path: the feature is built once, and a build
    /// that got the mode wrong should say so on every machine rather than only where a developer
    /// left the asserts in.
    inline NVSDK_NGX_PerfQuality_Value ngxQualityOf(Upscale upscale)
    {
        switch (upscale)
        {
            case Upscale::Performance:
                return NVSDK_NGX_PerfQuality_Value_MaxPerf;
            case Upscale::Balanced:
                return NVSDK_NGX_PerfQuality_Value_Balanced;
            case Upscale::Quality:
                return NVSDK_NGX_PerfQuality_Value_MaxQuality;
            case Upscale::Dlaa:
                return NVSDK_NGX_PerfQuality_Value_DLAA;
            case Upscale::Off:
                break;
        }

        throw Error("Ray Reconstruction was asked to build for an upscale mode that is the absence of one");
    }

    /// The network a preset selects, as NGX numbers them.
    ///
    /// **Ray Reconstruction's own enum, and not super-resolution's.** `nvsdk_ngx_defs_dlssd.h`
    /// retires A through C and names D and E; the enum of the same shape in `nvsdk_ngx_defs.h`
    /// retires D as well and names J through M. They are different networks reached through
    /// different parameters, and a value from one handed to the other is a preset the library does
    /// not recognise — which it answers by reverting to the default, silently, which is the state
    /// this exists to leave.
    inline NVSDK_NGX_RayReconstruction_Hint_Render_Preset ngxPresetOf(Preset preset)
    {
        switch (preset)
        {
            case Preset::D:
                return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D;
            case Preset::E:
                return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E;
            case Preset::Default:
                return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default;
        }

        return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default;
    }

    /// Which parameter carries the preset hint for a quality level.
    ///
    /// **One hint per quality level, because NGX keeps one network per level.** The feature is built
    /// for exactly one of them, so exactly one of these is worth setting; setting the rest would be
    /// stating a preference about features this renderer never creates.
    ///
    /// `Off` is refused here too, for the reason `ngxQualityOf` gives: a preset hint for a feature
    /// that is not being built names a network nothing will run.
    inline const char* ngxPresetParameterOf(Upscale upscale)
    {
        switch (upscale)
        {
            case Upscale::Performance:
                return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance;
            case Upscale::Balanced:
                return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced;
            case Upscale::Quality:
                return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality;
            case Upscale::Dlaa:
                return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA;
            case Upscale::Off:
                break;
        }

        throw Error("Ray Reconstruction was asked for the preset of an upscale mode that is the absence of one");
    }
}
