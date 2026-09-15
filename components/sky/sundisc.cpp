#include "sundisc.hpp"

#include <algorithm>

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
}
