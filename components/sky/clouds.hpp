#pragma once

#include <string_view>

#include <osg/Vec4f>

namespace Sky
{
    /// What Morrowind lights its cloud deck with.
    ///
    /// The weather's fog colour with an eighth added, out of `MWRender::SkyManager::setWeather`.
    /// **In the space the file records it**, because that is where the engine adds it: the same lift
    /// applied to linear light is a different colour rather than the same one brightened, and the
    /// two renderers have to arrive at the same cloud.
    osg::Vec4f cloudColour(const osg::Vec4f& fog);

    /// What a weather names its cloud texture, as the content files spell it — a bare file name,
    /// which the archive holds under `textures/`. Empty where the weather names none, which the
    /// shipped fallbacks do for ash and blight.
    std::string_view cloudTexture(std::string_view weather);

    /// How fast that weather's deck scrolls, out of `Weather_<name>_Cloud_Speed`. It is the only
    /// thing that differs between a still overcast and a scudding storm.
    float cloudSpeed(std::string_view weather);

    /// How much of a weather's transition the deck's own crossing is spread over —
    /// `Weather_<name>_Clouds_Maximum_Percent`. **Two of the ten record none**, which is not a rate.
    float cloudsMaximumPercent(std::string_view weather);

    /// How far the deck has crossed from one weather's texture to the next.
    ///
    /// **Not the plain transition factor.** Each weather spreads its own arrival over a share of the
    /// crossing, so a storm's sky can roll in ahead of its light — and where the content records
    /// none of that share, the deck crosses at once rather than dividing by nothing. A NaN here is
    /// a black sky: the rasterizer survives one because a NaN opacity draws nothing and the old sky
    /// stays, and a tracer mixes its whole sky by it.
    ///
    /// @param transitionRatio how far the weather itself has crossed, from nought to one.
    float cloudBlend(float transitionRatio, float cloudsMaximumPercent);

    /// Whether the deck moves on the world's clock or on the player's — `Weather_Timescale_Clouds`.
    bool timescaleClouds();
}
