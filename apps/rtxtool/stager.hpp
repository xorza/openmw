#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace MWBase
{
    class World;
}

namespace RtxTool
{
    struct SessionRequest;
    struct Stop;

    /// Puts the world where a stop stands, once, at its start: the player in the stop's cell, the
    /// clock and the sky, the seed, the rate the clock runs at, god mode, the interface, and the walls
    /// where the camera is flown or followed rather than walked. What moves while the stop runs is
    /// `CameraDriver`'s.
    class Stager
    {
    public:
        /// Stages `stop` of `request`, or says why it cannot be: a cell nothing is called. Ends with
        /// the navmesh built whole and the renderer's history let go of, so the first frame drawn
        /// after this is drawn at the stop and from nothing before it.
        std::optional<std::string> stage(const Stop& stop, const SessionRequest& request) const;

        /// Tells the renderer that nothing before this frame describes where it now stands.
        static void forgetHistory();

        /// Puts the sky under the weather called `name` over the player's region, as `changeweather`
        /// would, and warns for a name that is none of the ten.
        static void setWeather(MWBase::World& world, std::string_view name);

    private:
        /// Gives the player every attribute and skill at 255, a Speed of 2000, level 255 and a
        /// million gold, through the calls the console's `setspeed`, `setlevel` and `additem` make.
        /// A body walking at Morrowind's pace crosses a cell in a minute.
        static void boostPlayer();

        /// Turns the player's collision off, as `tcl` does.
        static void turnCollisionOff(MWBase::World& world);
    };
}
