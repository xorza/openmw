#include "rtxsettings.hpp"

#include <components/rtx/cellgrid.hpp>
#include <components/rtx/upscale.hpp>
#include <components/settings/values.hpp>

namespace MWRender
{
    RtxSettingValues RtxSettingValues::fromRegistry()
    {
        return RtxSettingValues{
            .mUpscale = Settings::rtx().mUpscale.get(),
            .mPreset = Settings::rtx().mPreset.get(),
            .mReflex = Settings::rtx().mReflex.get(),
            .mDistantLandCells = Settings::rtx().mDistantLandCells,
            .mViewingDistance = Settings::camera().mViewingDistance,
            .mObjectPaging = Settings::terrain().mObjectPaging,
            .mObjectPagingMinSize = Settings::terrain().mObjectPagingMinSize,
        };
    }

    RtxSettings RtxSettings::derive(const RtxSettingValues& values)
    {
        return RtxSettings{
            .mUpscaling = {
                .mMode = Rtx::sUpscaleNames.require(values.mUpscale, "an upscale mode"),
                .mPreset = Rtx::sPresetNames.require(values.mPreset, "a Ray Reconstruction preset"),
            },
            .mLatency = Rtx::sLatencyModeNames.require(values.mReflex, "a Reflex mode"),
            .mMirror = {
                .mReach = Rtx::distantLandReach(values.mDistantLandCells, values.mViewingDistance),
                .mDistantStatics = values.mObjectPaging,
                .mMinSize = values.mObjectPagingMinSize,
            },
        };
    }
}
