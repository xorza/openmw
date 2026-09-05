#include "framerequest.hpp"

namespace RtxTool
{
    StagingRequest FrameRequest::describeStaging(
        const std::optional<osg::Vec3f>& origin, const std::optional<osg::Vec3f>& target) const
    {
        return StagingRequest{
            .mWeather = mWeather,
            .mHour = mHour,
            .mDay = mDay,
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
