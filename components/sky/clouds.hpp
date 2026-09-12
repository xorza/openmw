#pragma once

#include <string_view>

namespace Sky
{
    /// What a weather names its cloud texture, as the content files spell it — a bare file name,
    /// which the archive holds under `textures/`. Empty where the weather names none, which the
    /// shipped fallbacks do for ash and blight.
    ///
    /// **Read here rather than in the game's weather manager**, because the ray tracer's sky is
    /// built from the ten weathers' sheets at startup and has no weather manager to ask.
    std::string_view cloudTexture(std::string_view weather);
}
