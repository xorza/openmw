#include "framerequest.hpp"

#include "stagedworld.hpp"

namespace RtxTool
{
    Rtx::RendererOptions FrameRequest::describeRenderer(
        const Rtx::ValidationOptions& validation, SDL_Window* window) const
    {
        return Rtx::RendererOptions{
            .mShaderDirectory = mShaderDirectory,
            .mWidth = mWidth,
            .mHeight = mHeight,
            .mUpscale = mUpscale,
            .mPreset = mPreset,
            .mWindow = window,
            .mValidation = validation,
        };
    }

    StagingRequest FrameRequest::describeStaging(std::optional<float> hour, const std::optional<osg::Vec3f>& origin,
        const std::optional<osg::Vec3f>& target) const
    {
        return StagingRequest{
            .mWeather = mWeather,
            .mHour = hour.value_or(mHour),
            .mDay = mDay,
            .mFieldOfView = mFieldOfView,
            .mOrigin = origin,
            .mTarget = target,
        };
    }
}
