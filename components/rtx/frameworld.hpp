#pragma once

#include <array>
#include <limits>

#include <osg/Vec3f>

#include "fogbuilder.hpp"
#include "lightbuilder.hpp"
#include "moonbuilder.hpp"
#include "scenedesc.hpp"
#include "shaders/visibility.h"
#include "skybuilder.hpp"

namespace Weather
{
    class Precipitation;
}

namespace Rtx
{
    /// What the world is doing this frame, in the units the renderer takes.
    ///
    /// **One list of the frame's world half, and one place that writes it.** The game and the
    /// harness reach these numbers by different routes — one reports what a live weather system
    /// settled on, the other derives them from the content files at an hour it was told — and that
    /// difference is real and stays. What was not real is that each then wrote them into
    /// `VisibilityConstants` itself, twenty-odd assignments apart, against no shared test: a field
    /// added to one and forgotten in the other is a `shot` that quietly stops predicting the game.
    ///
    /// Three of those have already happened. The sea's clock was filled by the harness and left at
    /// zero by the game, so every wave stood still in the game alone. Both moons had to be added
    /// twice in one sitting. And `mAir.mUniform` — whether the air is an even
    /// haze or banked — was written only by the harness, so every interior in the *game* ran the
    /// outdoor coverage field a room is far too small for.
    ///
    /// **Everything here is already in the renderer's units**: colours linear, fog an extinction
    /// rather than two distances, the weather blend the right way round, the moons placed. What each
    /// side does to get here is its own business; what happens after is not.
    /// How hard a fall of weather rains on the water, from nought to one.
    ///
    /// **The precipitation's own alpha where its kind rings the surface, and nought where it does
    /// not** — which is the number the rasterizer hands its water as `rainIntensity`, and
    /// `Weather::Precipitation::ripplesEnabled` is what says whether a kind rings: rain does and snow
    /// settles, off the ini's own `Rain Ripples` and `Snow Ripples`.
    /// @param fall what is falling, or null for a world with no weather over it.
    float rainOnWater(const Weather::Precipitation* fall);

    /// The deck and the star field a world with no sky has: nothing to draw, which the shader reads
    /// off the texture slot before it samples anything.
    ///
    /// **Built whole and then named, rather than by designated initializer**, so that every other
    /// field is value-initialised where the compiler can see it. A designated initializer does the
    /// same, but GCC cannot tell it from an aggregate left short, and the game's own translation
    /// units are built with that warning on.
    Shaders::CloudDeck noDeck();
    Shaders::StarField noStars();

    /// A patch the sky skips: straight up, no size and no texture.
    ///
    /// **Written out rather than left to `{}`.** `Shaders::SkyPatch` is a shader struct with no
    /// defaults of its own, so a value-initialised one names texture slot zero — a real texture,
    /// belonging to whatever the scene put there. Only its radius of nothing kept it off the screen.
    Shaders::SkyPatch noPatch();

    /// Six of those, which is the whole sky a world with none has.
    std::array<Shaders::SkyPatch, Shaders::SKY_PATCH_COUNT> noPatches();

    struct FrameWorld
    {

        /// The sun, and it is a sun or it is nothing.
        ///
        /// **Built by `makeSkylight` and never assembled field by field.** Its irradiance is zero
        /// exactly when there is no sun to see, so everything the sun does downstream hangs off one
        /// test — which is what stops a shadow being cast out of a sky with no sun drawn in it.
        Sun mSun;

        /// The cell's own ambient, linear. What a path is terminated with rather than what is added
        /// on top of it — `visibility.h` says why.
        ///
        /// **A night's carries the sun**, because Morrowind's night sun is not one: `makeSkylight`
        /// puts what the file left in that slot here, where light with no direction belongs.
        osg::Vec3f mAmbient;

        /// How much of that arrives from the sky — one out of doors and nothing in a room.
        /// `Shaders::VisibilityConstants::mAmbientFromSky` says what turns on it.
        float mAmbientFromSky = 0.0f;

        /// What a ray that hit nothing comes back with, at the horizon and overhead. The game
        /// records one colour for the fog and the sky's lower half because they are the same thing
        /// at two distances, so the horizon is the air's own colour and not a third number.
        osg::Vec3f mSkyHorizon;
        osg::Vec3f mSkyZenith;

        /// What the sky lights with beyond those two. `Rtx::skyFill` says why a night has one and a
        /// day has none.
        osg::Vec3f mSkyFill;

        /// The air between the eye and everything else, as a medium.
        Fog mAir;

        /// Where the water's surface is, or minus infinity where the cell holds none — so that
        /// "how deep is this point" is never positive and nothing downstream needs a second
        /// question.
        float mWaterLevel = -std::numeric_limits<float>::infinity();

        /// How long the water has been moving, in seconds. Zero is a still sea and a repeatable
        /// frame, which is what a screenshot wants.
        float mSeconds = 0.0f;

        /// How hard it rains on the water, from nought to one, out of `rainOnWater`.
        float mRainOnWater = 0.0f;

        /// The cloud deck and the star field, already in the units the shader takes: `describeClouds`
        /// and `describeStars` are what both renderers reach them through.
        Shaders::CloudDeck mClouds = noDeck();
        Shaders::StarField mStars = noStars();

        /// The nebulae and constellations painted across the star sphere. `describePatches` fills
        /// them; the default is `noPatches`, which is a room's night sky.
        std::array<Shaders::SkyPatch, Shaders::SKY_PATCH_COUNT> mSkyPatches = noPatches();

        /// Masser and Secunda, in that order. An alpha of nothing is a moon the sky skips, which is
        /// what an interior and an interface trace both leave behind.
        std::array<MoonPlacement, 2> mMoons;
    };

    /// Writes the world's half of a frame into the constants it is traced with.
    ///
    /// The camera's half is `makeCamera*`'s and is expected to be there already.
    void applyWorld(const FrameWorld& world, Shaders::VisibilityConstants& constants);

    /// What a frame's sky, air and water are, as far as neither host can work it out for the other.
    ///
    /// **A `Daylight` and the handful of things a `Daylight` does not carry.** How each host reaches
    /// one is its own business and stays so: the game reports what a live weather system settled on,
    /// and the harness derives it from the content files at an hour it was told. What follows is
    /// arithmetic over that, and `describeWorld` is where it happens once.
    struct WorldReading
    {
        Daylight mDaylight;

        /// Whether there is a sky over this cell at all. Nothing under `false` draws a deck, a star,
        /// a patch or a moon, and nothing measures a sky budget it has no sky for.
        bool mOutdoors = false;

        /// Whether the air takes its colour from the dome it stands in.
        ///
        /// **Not the same question as `mOutdoors`, and a quasi-exterior is where they part.** Such a
        /// cell has weather over it and a room's air in it: Morrowind records its fog in the cell
        /// rather than in the weather, so the dome's mean must not be mixed into a colour the
        /// content already stated.
        bool mFogFromSky = false;

        /// The weather's `Glare_View`, which is what keeps the stars in under an overcast.
        float mGlare = 1.0f;

        /// How far the star sphere has turned and the deck has scrolled — `Sky::SkyRoll`'s two.
        float mStarRoll = 0.0f;
        float mCloudRoll = 0.0f;

        /// Where the sky's own sheets sit in the scene's texture table.
        SkyContent mSky;

        /// Masser and Secunda, placed and with their faces named. **An input and not a derivation**:
        /// the game is handed the angles by its weather system and the harness works them out from a
        /// date, and neither can do the other's.
        std::array<MoonPlacement, 2> mMoons;

        std::uint32_t mWeather = Shaders::WEATHER_CLEAR;
        std::uint32_t mNextWeather = Shaders::WEATHER_CLEAR;

        /// How far the *deck* has crossed, which is not how far the weather has.
        float mCloudBlend = 0.0f;

        /// Which way each weather's sheet is turned, on the same terms as `mMoons`.
        osg::Vec3f mCloudDirection;
        osg::Vec3f mNextCloudDirection;

        float mWaterLevel = -std::numeric_limits<float>::infinity();
        float mSeconds = 0.0f;
        float mRainOnWater = 0.0f;
    };

    /// Assembles the frame's world half out of what a host read.
    ///
    /// **The order is the whole of what this is for.** The stars have to be known before the sky's
    /// budget, because what the sheets add is measured out of the weather's ambient; the budget
    /// before the air, because the air is lit by the dome it stands in; and both before the deck,
    /// which is lit by the dome and by the moons under it. Written out at each host, that order was
    /// four chances for one of them to drift — and `FrameWorld` above lists three times it already
    /// had.
    FrameWorld describeWorld(const WorldReading& reading);
}
