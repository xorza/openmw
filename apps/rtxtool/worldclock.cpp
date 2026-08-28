#include "worldclock.hpp"

#include <algorithm>
#include <cmath>

namespace RtxTool
{
    WorldClock::WorldClock(int day, float hour)
        : mDay(day)
        , mHour(hour)
    {
    }

    void WorldClock::advance(float realSeconds)
    {
        mStep = std::min(realSeconds, sLongestStep);
        mWallSeconds += mStep;

        if (!mRunning)
        {
            mWorldSeconds += mStep;
            return;
        }

        mWorldSeconds += mStep * sSpeed;
        nudgeHour(mStep * sHoursPerSecond);
    }

    void WorldClock::nudgeHour(float hours)
    {
        // Wrapped rather than clamped, so holding a key walks the sun round and round instead of
        // parking it at a horizon — and wrapped twice, because `fmod` keeps the sign of a step back.
        mHour = std::fmod(std::fmod(mHour + hours, 24.0f) + 24.0f, 24.0f);
    }

    void WorldClock::nudgeDay(int days)
    {
        mDay = std::max(mDay + days, 0);
    }
}
