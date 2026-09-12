#include "clouds.hpp"

#include <string>

#include <components/fallback/fallback.hpp>

namespace Sky
{
    std::string_view cloudTexture(std::string_view weather)
    {
        return Fallback::Map::getString("Weather_" + std::string(weather) + "_Cloud_Texture");
    }
}
