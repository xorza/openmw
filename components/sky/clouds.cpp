#include "clouds.hpp"

#include <string>

#include <components/fallback/fallback.hpp>

namespace Sky
{
    namespace
    {
        /// What the engine adds to the fog before lighting its clouds with it. Flat across the three
        /// channels, and the alpha is left alone because the deck's own alpha is its coverage.
        constexpr float sLift = 0.13f;
    }

    osg::Vec4f cloudColour(const osg::Vec4f& fog)
    {
        return fog + osg::Vec4f(sLift, sLift, sLift, 0.0f);
    }

    std::string_view cloudTexture(std::string_view weather)
    {
        return Fallback::Map::getString("Weather_" + std::string(weather) + "_Cloud_Texture");
    }

    float cloudSpeed(std::string_view weather)
    {
        return Fallback::Map::getFloat("Weather_" + std::string(weather) + "_Cloud_Speed");
    }

    float cloudsMaximumPercent(std::string_view weather)
    {
        return Fallback::Map::getFloat("Weather_" + std::string(weather) + "_Clouds_Maximum_Percent");
    }

    float cloudBlend(float transitionRatio, float cloudsMaximumPercent)
    {
        // Nought is not a rate. The shipped fallbacks give ash and blight no maximum percent, so a
        // transition into either divided by nothing; nothing recorded means the deck crosses at once.
        if (!(cloudsMaximumPercent > 0.0f))
            return 1.0f;

        return transitionRatio / cloudsMaximumPercent;
    }

    bool timescaleClouds()
    {
        return Fallback::Map::getBool("Weather_Timescale_Clouds");
    }
}
