#ifndef OPENMW_COMPONENTS_SKY_SUNDISC_H
#define OPENMW_COMPONENTS_SKY_SUNDISC_H

#include <osg/Vec3f>

namespace Sky
{
    struct TimeOfDaySettings;

    /// Whether the sun is drawn at `hour`: `MWWorld::WeatherManager::update`'s own gate, which hides
    /// the disc from the night's start to its end. Lifted here so the renderer that asks whether
    /// there is a sun to cast a shadow from reads the rule the weather manager draws by.
    bool sunUp(float hour, const TimeOfDaySettings& times);

    /// How much of the disc is there at `hour`, as `MWWorld::WeatherManager::calculateResult` writes
    /// it into the disc colour's alpha: squared out across dusk, linear in over the first half of
    /// the sunrise window — the hour past dawn and not a fraction of it, so a window longer than an
    /// hour passes one — and one through the day. Runs on through the night and comes back at one,
    /// because a rasterizer that has hidden the disc has no use for the answer; `sunUp` is the
    /// other half.
    float sunDiscAlpha(float hour, const TimeOfDaySettings& times);

    /// Where the disc is drawn for the orbit's `direction`: the direction reversed, and its height
    /// bent down as it leaves the zenith by Morrowind's own rule, which `RenderingManager` applied
    /// before handing the dome its sun. One statement, because the light and the disc are placed
    /// by two renderers off one direction, and the disc has to be where the shadows say it is.
    osg::Vec3f sunDiscPosition(const osg::Vec3f& direction);
}

#endif
