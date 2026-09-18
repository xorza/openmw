#include "sundisc.hpp"

#include <algorithm>
#include <cmath>

#include "timeofday.hpp"

namespace Sky
{
    bool sunUp(const float hour, const TimeOfDaySettings& times)
    {
        return !(hour >= times.mNightStart || hour <= times.mNightEnd);
    }

    float sunDiscAlpha(const float hour, const TimeOfDaySettings& times)
    {
        if (hour >= times.mDayEnd)
        {
            // sunset
            const float fade = std::min(1.f, (hour - times.mDayEnd) / (times.mNightStart - times.mDayEnd));
            return 1.f - fade * fade;
        }

        // sunrise
        if (hour >= times.mNightEnd && hour <= times.mNightEnd + times.mSunriseDuration / 2.f)
            return hour - times.mNightEnd;

        return 1.f;
    }

    osg::Vec3f sunDiscPosition(const osg::Vec3f& direction)
    {
        osg::Vec3f position = -direction;

        // This is based on the exterior sun orbit and won't make sense for interiors, see WeatherManager::update
        position.z() = 400.f - std::abs(position.x());

        return position;
    }
}
