#include "lightcontroller.hpp"

#include <cmath>

#include <components/misc/rng.hpp>

namespace SceneUtil
{

    LightController::LightController()
        : mType(LT_Normal)
        , mPhase(0.25f + Misc::Rng::rollClosedProbability() * 0.75f)
        , mBrightness(0.675f)
        , mStartTime(0.0)
        , mLastTime(0.0)
        , mTicksToAdvance(0.f)
    {
    }

    float LightController::advance(double simulationTime)
    {
        if (mType == LT_Normal)
            return 1.f;

        if (mStartTime == 0)
            mStartTime = simulationTime;

        // Updating flickering at 15 FPS like vanilla.
        constexpr float updateRate = 15.f;
        mTicksToAdvance = static_cast<float>(simulationTime - mStartTime - mLastTime) * updateRate * 0.25f
            + mTicksToAdvance * 0.75f;
        mLastTime = simulationTime - mStartTime;

        float speed = (mType == LT_Flicker || mType == LT_Pulse) ? 0.1f : 0.05f;
        if (mBrightness >= mPhase)
            mBrightness -= mTicksToAdvance * speed;
        else
            mBrightness += mTicksToAdvance * speed;

        if (std::abs(mBrightness - mPhase) < speed)
        {
            if (mType == LT_Flicker || mType == LT_FlickerSlow)
                mPhase = 0.25f + Misc::Rng::rollClosedProbability() * 0.75f;
            else // if (mType == LT_Pulse || mType == LT_PulseSlow)
                mPhase = mPhase <= 0.5f ? 1.f : 0.25f;
        }

        return mBrightness;
    }

}
