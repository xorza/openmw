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
            .mReorder = mReorder,
        };
    }

    StagingRequest FrameRequest::describeStaging(
        const std::optional<osg::Vec3f>& origin, const std::optional<osg::Vec3f>& target) const
    {
        return StagingRequest{
            .mWeather = mWeather,
            .mHour = mHour,
            .mDay = mDay,
            .mFieldOfView = mFieldOfView,
            .mOrigin = origin,
            .mTarget = target,
        };
    }

    StagingRequest FrameRequest::describeStaging(const View& view) const
    {
        StagingRequest request = describeStaging(view.mOrigin, view.mTarget);
        request.mWeather = view.mWeather.value_or(mWeather);
        request.mHour = view.mHour.value_or(mHour);

        return request;
    }
}
