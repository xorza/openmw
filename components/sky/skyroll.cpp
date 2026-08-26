#include "skyroll.hpp"

#include <cmath>

namespace Sky
{
    namespace
    {
        /// What the engine divides a weather's cloud speed by, and what a real minute is worth to it.
        /// Morrowind's own, out of `MWRender::SkyManager::update`.
        constexpr float sCloudSpeedScale = 400.0f;
        constexpr float sSecondsPerMinute = 60.0f;

        /// The scroll wraps here, which is the range the engine's texture matrix runs over.
        constexpr float sCloudWrap = 4.0f;

        /// How long the stars take to come round, in game seconds: four days of ninety-six hours'
        /// worth of seconds, which is how the engine spells it.
        constexpr float sStarPeriod = 3600.0f * 96.0f;

        constexpr float sTau = 6.283185307179586f;
    }

    void SkyRoll::advance(float seconds, float cloudSpeed, float timeScale, bool timescaleClouds)
    {
        float scrolled = seconds * cloudSpeed / sCloudSpeedScale;
        if (timescaleClouds)
            scrolled *= timeScale / sSecondsPerMinute;

        // `fmod` rather than one subtraction, which is what the engine does: a frame long enough to
        // scroll past four — a loading pause, or a `--seconds` the harness ran in one step — would
        // leave the timer outside its range for as long as it took to come back round.
        mClouds = std::fmod(mClouds + scrolled, sCloudWrap);
        if (mClouds < 0.0f)
            mClouds += sCloudWrap;

        mStars = std::fmod(mStars + timeScale * seconds * sTau / sStarPeriod, sTau);
    }
}
