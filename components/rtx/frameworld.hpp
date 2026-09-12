#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <osg/Vec3f>

#include "fogbuilder.hpp"
#include "moonbuilder.hpp"
#include "shaders/visibility.h"
#include "skybuilder.hpp"
#include "skylight.hpp"

namespace osg
{
    class Node;
}

namespace Rtx
{
    class SceneExtractor;

    /// Walks what the weather drops, which is a second root to whoever mirrors the world. Those
    /// nodes hang under the sky's camera-relative transform, so their particles are placed about
    /// the origin and stood at the eye the drops were driven with — the one place the box travels
    /// nowhere, so a sprite's travel between two frames is its fall. Not walked where the eye is
    /// under water: the sky manager freezes the drops where they stand, and walked anyway they hang
    /// in the air.
    ///
    /// @param fall what is falling — the sky manager's rain box or its driven effect — or null for
    ///        a world with nothing of that kind over it.
    /// @param frameNumber the frame the walk belongs to, as `SceneExtractor::extractWorld` takes it.
    void mirrorPrecipitation(
        SceneExtractor& extractor, osg::Node* fall, const osg::Vec3f& eye, bool underwater, std::size_t frameNumber);

    /// The deck and the star field a world with no sky has: nothing to draw, which the shader reads
    /// off the texture slot before it samples anything. Built whole and then named rather than by
    /// designated initializer, which GCC cannot tell from an aggregate left short.
    Shaders::CloudDeck noDeck();
    Shaders::StarField noStars();

    /// A patch the sky skips: straight up, no size and no texture. Written out rather than left to
    /// `{}`, because a value-initialised `Shaders::SkyPatch` names texture slot zero.
    Shaders::SkyPatch noPatch();

    /// What a frame's sky, air and water are, as far as neither host can work it out for the other:
    /// a `Daylight` and the handful of things a `Daylight` does not carry.
    struct WorldReading
    {
        Daylight mDaylight;

        /// Whether there is a sky over this cell at all. Nothing under `false` draws a deck, a star,
        /// a patch or a moon, or lights its air by a dome. True of a quasi-exterior, which is an
        /// interior cell the engine runs the weather system for.
        bool mOutdoors = false;

        /// The weather's `Glare_View`, which is what keeps the stars in under an overcast.
        float mGlare = 1.0f;

        /// How far the star sphere has turned and the deck has scrolled — the sky manager's two clocks.
        float mStarRoll = 0.0f;

        /// Where the sky's own sheets sit in the scene's texture table.
        SkyContent mSky;

        /// Masser and Secunda, placed and with their faces named. **An input and not a derivation**:
        /// the angles come from the weather system, and nothing here can work them out.
        std::array<MoonPlacement, 2> mMoons;

        /// Which weather is over the eye and which is arriving, and how the two decks stand.
        CloudCrossing mClouds;

        float mWaterLevel = -std::numeric_limits<float>::infinity();
        float mSeconds = 0.0f;
        float mRainOnWater = 0.0f;
    };

    /// Writes the frame's world half into the constants it is traced with, and answers what to hold
    /// the frame's measured exposure back by — `Skylight::mExposureBias`, carried. The camera's
    /// half is `makeCamera*`'s and is left alone. The order is the whole of what this is for: the
    /// stars before the sky's budget, the budget before the air, and both before the deck. One
    /// call and not twenty assignments per host, or a field added to one host is forgotten in the
    /// other.
    float describeWorld(const WorldReading& reading, Shaders::VisibilityConstants& constants);
}
