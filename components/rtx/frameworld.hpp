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

namespace Weather
{
    class Precipitation;
}

namespace Rtx
{
    class SceneExtractor;

    /// How hard a fall of weather rains on the water, from nought to one.
    ///
    /// **The precipitation's own alpha where its kind rings the surface, and nought where it does
    /// not.** `Weather::Precipitation::ripplesEnabled` is what says whether a kind rings: rain does
    /// and snow settles, off the ini's own `Rain Ripples` and `Snow Ripples`.
    ///
    /// **The same number the rasterizer hands its water, and not the same expression.**
    /// `SkyManager::getRainRipplesEnabled` asks this with the sky's own switch in front of it, which
    /// changes no answer: `setEnabled(false)` clears the downpour in the same call, and an interior
    /// is the only thing that turns the sky off.
    ///
    /// @param fall what is falling, or null for a world with no weather over it.
    float rainOnWater(const Weather::Precipitation* fall);

    /// Walks what the weather drops, which is a second root to whoever mirrors the world.
    ///
    /// **One call, because the shape of this walk is nobody's to choose.** Those nodes hang under
    /// the sky's camera-relative transform, which carries no translation — so their particles are
    /// placed about the origin and the eye is what stands them back in the world. Anchored, masked
    /// and gated at the caller instead, that is four decisions a second caller could take
    /// differently.
    ///
    /// **Stood at the eye the drops were driven with**, because that is the one place the box
    /// travels nowhere: every step of that eye is taken back out of the drops again, so a sprite's
    /// own travel between two frames is its fall and the reprojection can be handed it as such. The
    /// rasterizer stands the box at the camera it draws from, and the game hands this the same eye
    /// — `Weather::Conditions::mEye` says where.
    ///
    /// **Nothing falls where the eye is under water, and stopping it is not hiding it.**
    /// `Weather::Precipitation` freezes the drops where they stand and leaves what to draw to
    /// whoever is drawing: the rasterizer answers by not culling the subtree and a ray tracer by not
    /// walking it. Walked anyway, the drops the surface was crossed with hang in the air, frozen,
    /// for as long as the eye stays under it.
    ///
    /// @param fall what is falling, or null for a world with no weather over it.
    /// @param frameNumber the frame the walk belongs to, as `SceneExtractor::extractWorld` takes it.
    void mirrorPrecipitation(SceneExtractor& extractor, Weather::Precipitation* fall, std::size_t frameNumber);

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

    /// What a frame's sky, air and water are, as far as neither host can work it out for the other.
    ///
    /// **A `Daylight` and the handful of things a `Daylight` does not carry.** How a host reaches
    /// one is its own business: the game reports what its live weather system settled on. What
    /// follows is arithmetic over that, and `describeWorld` is where it happens once.
    struct WorldReading
    {
        Daylight mDaylight;

        /// Whether there is a sky over this cell at all. Nothing under `false` draws a deck, a star,
        /// a patch or a moon, nothing measures a sky budget it has no sky for, and nothing lights
        /// its air by a dome it stands under none of.
        ///
        /// **True of a quasi-exterior, which is the whole of what one is.** Vivec's cantons and
        /// Mournhold are interior cells the engine runs the weather system for, so every one of
        /// those is decided for them exactly as it is out of doors.
        bool mOutdoors = false;

        /// The weather's `Glare_View`, which is what keeps the stars in under an overcast.
        float mGlare = 1.0f;

        /// How far the star sphere has turned and the deck has scrolled — `Sky::SkyRoll`'s two.
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
    /// the frame's measured exposure back by — `Skylight::mExposureBias`, carried.
    ///
    /// The camera's half is `makeCamera*`'s and is expected to be there already. Every camera field
    /// is left alone.
    ///
    /// **The order is the whole of what this is for.** The stars have to be known before the sky's
    /// budget, because what the sheets add is measured out of the weather's ambient; the budget
    /// before the air, because the air is lit by the dome it stands in; and both before the deck,
    /// which is lit by the dome and by the moons under it. Written out at each host, that order is
    /// four chances for one of them to drift.
    ///
    /// **One call, and not a struct a host copies out field by field.** Twenty-odd assignments per
    /// host are a field added to one host and forgotten in the other — a sea's clock standing
    /// still in the game alone, a moon added twice, the outdoor coverage field running in every
    /// interior.
    float describeWorld(const WorldReading& reading, Shaders::VisibilityConstants& constants);
}
