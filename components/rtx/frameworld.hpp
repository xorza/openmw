#pragma once

#include <array>
#include <limits>

#include <osg/Vec3f>

#include "fogbuilder.hpp"
#include "moonbuilder.hpp"
#include "scenedesc.hpp"
#include "shaders/visibility.h"

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
        Shaders::CloudDeck mClouds{ .mOpacity = 0.0f, .mTexture = Shaders::NO_TEXTURE, .mNext = Shaders::NO_TEXTURE };
        Shaders::StarField mStars{ .mTexture = Shaders::NO_TEXTURE };

        /// The nebulae and constellations painted across the star sphere. `describePatches` fills
        /// them; a default leaves every one with no texture, which is a room's night sky.
        std::array<Shaders::SkyPatch, Shaders::SKY_PATCH_COUNT> mSkyPatches{};

        /// Masser and Secunda, in that order. An alpha of nothing is a moon the sky skips, which is
        /// what an interior and an interface trace both leave behind.
        std::array<MoonPlacement, 2> mMoons;
    };

    /// Writes the world's half of a frame into the constants it is traced with.
    ///
    /// The camera's half is `makeCamera*`'s and is expected to be there already.
    void applyWorld(const FrameWorld& world, Shaders::VisibilityConstants& constants);
}
