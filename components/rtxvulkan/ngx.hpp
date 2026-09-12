#pragma once

#include <string>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>

#include <components/rtx/error.hpp>
#include <components/rtx/reconstruction.hpp>

#include "dlss.hpp"

namespace Rtx
{
    /// NGX's own name for a result code, with the code after it. Asked rather than switched on,
    /// because the SDK names codes this build was not told about.
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

    /// The quality level an upscale setting is, as NGX numbers them. `Off` is refused rather than
    /// answered, because grouping it with `Performance` once made a contradiction into the
    /// fastest, softest mode this renderer has, on the path a frame budget is measured against.
    /// Thrown rather than asserted because the feature is built once, on every machine.
    inline NVSDK_NGX_PerfQuality_Value ngxQualityOf(Upscale upscale)
    {
        switch (upscale)
        {
            case Upscale::UltraPerformance:
                return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
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

    /// The network a preset selects, as NGX numbers them — Ray Reconstruction's own enum in
    /// `nvsdk_ngx_defs_dlssd.h`, and not super-resolution's in `nvsdk_ngx_defs.h`, which names
    /// different networks and answers a value from the other by reverting to the default silently.
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

    /// Which parameter carries the preset hint for a quality level — one per level, because NGX
    /// keeps one network per level. `Off` is refused here too, for the reason `ngxQualityOf` gives.
    inline const char* ngxPresetParameterOf(Upscale upscale)
    {
        switch (upscale)
        {
            case Upscale::UltraPerformance:
                return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance;
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
