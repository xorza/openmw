#pragma once

#include <limits>

#include <osg/Vec3f>

#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/skybuilder.hpp>
#include <components/sky/skyroll.hpp>

namespace RtxTool
{
    /// How a cell is lit, which is the part of that a scene cannot carry.
    ///
    /// Its lamps are in the scene, being things standing in it. Its ambient belongs to the cell
    /// itself and its sun belongs to the hour, and neither is anywhere a ray can find them.
    struct CellLighting
    {
        osg::Vec3f mAmbient;

        /// How long the water has been moving, in seconds. Zero is a still sea and a deterministic
        /// frame, which is what a screenshot wants; a window passes its own clock.
        float mSeconds = 0.0f;

        /// Where the water's surface is. Minus infinity where the cell holds none, so that "how deep
        /// is this point" is never positive and nothing downstream needs a second question.
        float mWaterLevel = -std::numeric_limits<float>::infinity();

        /// The sun and the sky over an exterior. An interior leaves this dark.
        Rtx::Daylight mDaylight;

        /// Whether this cell has a sky over it.
        ///
        /// **The one thing `relight` cannot work out for itself.** An interior's ambient and air
        /// come out of its own `AMBI` record and owe nothing to the clock, so moving the hour must
        /// leave them exactly where they were rather than replacing them with an outdoor noon.
        bool mOutdoors = false;

        /// When the world stands, on Morrowind's own count of days from the one a new game begins
        /// and a twenty-four hour clock. **Only the moons read these** — where the sun is and what
        /// the air is doing were settled into `mDaylight` and `mFog` when the hour was chosen, and a
        /// moon cannot be, because its phase needs a date the sun never asked for.
        int mDay = 0;
        float mHour = 12.0f;

        /// Which weather the sky is under, which it is turning into, and how far along.
        ///
        /// **A settled sky names the same weather twice at a blend of nothing**, which is what a
        /// `shot` always is: nothing here advances on its own. A window can run a transition, and
        /// then these three say where it has got to — every colour, and the fog's depth, mixed at
        /// exactly the points `WeatherManager` mixes them.
        std::uint32_t mWeather = Rtx::Shaders::WEATHER_CLEAR;
        std::uint32_t mNextWeather = Rtx::Shaders::WEATHER_CLEAR;
        float mWeatherBlend = 0.0f;

        /// How hard that weather blows, as the content files record it. An interior has no wind.
        float mWindSpeed = 0.0f;

        /// How much of the sun this weather lets through, and how fast its deck runs. The engine
        /// mixes both across a transition, so both are settled when the weather is.
        float mGlare = 1.0f;
        float mCloudSpeed = 0.0f;

        /// How far the *deck* has crossed, which is not `mWeatherBlend`.
        ///
        /// **Each weather spreads its own arrival over a share of the crossing**, so a storm's sky
        /// rolls in ahead of its light. `Sky::cloudBlend` is the curve, and the game runs it too —
        /// crossing the sky linearly here was the harness quietly drawing a different transition.
        float mCloudBlend = 0.0f;

        /// Where the two moons' portraits sit in the scene's texture table. Whoever owns the scene
        /// puts them there; nothing about a cell decides it.
        Rtx::MoonFaces mFaces;

        /// And where the cloud decks and the star sheet sit, on the same terms.
        Rtx::SkyTextures mSky;

        /// How far the deck has scrolled and the stars have rolled. **Zero for a screenshot**, which
        /// is what makes one repeatable — the same choice `mSeconds` makes for the sea; a window
        /// advances it off its own clock.
        Sky::SkyRoll mRoll;

        /// The air in the cell, whichever of the two places it came from: an interior's `AMBI` or
        /// the weather over an exterior. A zero extinction is a cell with no fog, and costs nothing.
        Rtx::Fog mFog;
    };

    /// Writes how the cell is lit into the constants a frame is traced with.
    ///
    /// Shared because a screenshot and a window are the same frame: the two paths differ in where
    /// the camera comes from and in nothing else, and a light that reached one but not the other
    /// would be a difference nobody was looking for.
    ///
    /// The lamps themselves are not here. They are in the scene, and the pass finds them through the
    /// grid `SceneBuffers` binned them into rather than through a count anyone has to remember.
    void applyLighting(const CellLighting& lighting, Rtx::Shaders::VisibilityConstants& constants);

    /// Moves a cell's sky to another moment, leaving everything the sky does not decide.
    ///
    /// **What the window's clock keys turn, and it reloads nothing.** Where the sun and the moons
    /// stand and what the air is doing are arithmetic over the settings and the hour; the geometry,
    /// the lamps and the water are the same cell they were, so stepping an hour is a few dozen
    /// floating-point operations rather than a region being read again.
    ///
    /// An interior is left untouched: it has no sky for a clock to move.
    void relight(CellLighting& lighting, std::string_view weather, int day, float hour);

    /// The same, partway between two weathers — which is what a window running a transition wants.
    void relight(CellLighting& lighting, std::string_view from, std::string_view to, float blend, int day, float hour);
}
