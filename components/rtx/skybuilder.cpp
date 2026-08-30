#include "skybuilder.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <osg/Image>
#include <osg/Vec2f>

#include <components/misc/strings/lower.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sky/clouds.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "frameworld.hpp"
#include "lightbuilder.hpp"
#include "meantexel.hpp"
#include "shaders/colour.h"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// Where a storm drives, as the pair a rotation about the zenith is written with.
        ///
        /// **A swap and not an angle.** The engine turns its cloud mesh from due north onto the
        /// storm's direction, and turning a crossing back by that angle wants its cosine and its
        /// sine — which for a unit `(x, y)` measured from north are `y` and `x`. A direction nobody
        /// set is zero rather than north, and comes out as a sheet with no size, so the shader is
        /// given north instead.
        osg::Vec2f bearingOf(const osg::Vec3f& storm)
        {
            const osg::Vec2f flat(storm.y(), storm.x());

            return flat.length2() > 0.0f ? flat : osg::Vec2f(1.0f, 0.0f);
        }

        /// What a sheet averages: the luminance of what it paints, and how much of the sky it hides.
        ///
        /// Nothing where the file will not read or decode, which is the same answer a missing sheet
        /// gives and lands in the same place: a weather with no deck to draw.
        MeanTexel sheetMean(Resource::ImageManager& images, const VFS::Path::Normalized& path)
        {
            const osg::ref_ptr<const osg::Image> image = openImage(images, path);
            if (image == nullptr)
                return MeanTexel();

            return meanTexel(*image);
        }
    }

    std::uint32_t SkyContent::cloudsOf(std::uint32_t weather) const
    {
        if (weather >= mClouds.size() || mClouds[weather] == sNoIndex)
            return Shaders::NO_TEXTURE;

        return static_cast<std::uint32_t>(mClouds[weather]);
    }

    float SkyContent::meanOf(std::uint32_t weather) const
    {
        return weather < mCloudMean.size() ? mCloudMean[weather] : 0.0f;
    }

    float SkyContent::coverOf(std::uint32_t weather) const
    {
        return weather < mCloudCover.size() ? mCloudCover[weather] : 0.0f;
    }

    SkyContent addSkyContent(SceneDesc& scene, Resource::SceneManager& scenes)
    {
        const VFS::Manager& vfs = *scenes.getVFS();

        SkyContent loaded;
        loaded.mClouds.fill(sNoIndex);

        for (std::uint32_t weather = 0; weather < Shaders::WEATHER_COUNT; ++weather)
        {
            const std::string_view sheet = Sky::cloudTexture(weatherName(weather));
            if (sheet.empty())
                continue;

            // The file records a bare name and the archive holds it under `textures/`, which is the
            // same join the scene manager makes before it is handed one.
            const VFS::Path::Normalized path("textures/" + Misc::StringUtils::lowerCase(std::string(sheet)));
            if (!vfs.exists(path))
                continue;

            loaded.mClouds[weather] = scene.addTexture(path);
            scene.holdTexture(loaded.mClouds[weather]);

            // **Read here and not on the frame that needs it.** Averaging a 512-square sheet is a
            // quarter of a million texels, and there are six of them; the image is the one the
            // upload is about to take out of the same cache.
            const MeanTexel painted = sheetMean(*scenes.getImageManager(), path);
            loaded.mCloudMean[weather] = painted.opaque() * Shaders::LUMINANCE_WEIGHTS;
            loaded.mCloudCover[weather] = painted.mAlpha;
        }

        // **The shape the deck hangs on is the mesh's**, both of its numbers: how high the layer is
        // in tiles of its own sheet, and how far it falls away over the ground it covers.
        loaded.mShell = readCloudShell(scenes);

        // **The night sky is the mesh's**, every number of it: which sheet the field wears, how much
        // sky a tile of it covers, where it fades out, and where the six patches sit.
        loaded.mNight = readNightSky(scene, scenes);

        return loaded;
    }

    DeckLight deckLight(const Sun& sun, const osg::Vec3f& skyMean, std::span<const MoonPlacement, 2> moons)
    {
        // **The sky's own radiance, less what the deck keeps of it.** A layer under a hemisphere of
        // radiance `L` receives `pi L` and spreads what leaves its base over the hemisphere below,
        // so what comes back is `T * pi L / pi` — the `pi` divides out and a deck is simply a
        // fraction of the sky it hides.
        const osg::Vec3f fromSky = skyMean * Shaders::CLOUD_TRANSMISSION;

        // **A direction has to be turned into a level surface's share of it first.** The layer is
        // flat and the light is not overhead, so what lands is `E cos`, and what leaves the base is
        // that spread over the lower hemisphere.
        const auto sentDown = [](const osg::Vec3f& irradiance, const osg::Vec3f& towards) {
            return irradiance * (std::max(towards.z(), 0.0f) * Shaders::INV_PI * Shaders::CLOUD_TRANSMISSION);
        };

        osg::Vec3f direct = sentDown(sun.mIrradiance, sun.mPosition);
        for (const MoonPlacement& moon : moons)
            direct += sentDown(moon.mIrradiance, moon.mDirection);

        return DeckLight{ .mLit = fromSky + direct, .mShadowed = fromSky };
    }

    Shaders::CloudDeck describeClouds(std::uint32_t weather, std::uint32_t next, float blend, const DeckLight& light,
        const osg::Vec3f& storm, const osg::Vec3f& nextStorm, float scroll, const SkyContent& textures)
    {
        const std::uint32_t slot = textures.cloudsOf(weather);

        // **Written so a NaN lands on nought, which `std::clamp` does not do.** The blend comes off a
        // content file by way of a division, and a content file is untrusted: `clamp` compares and
        // hands back what it was given when both comparisons fail, so a NaN goes straight through it
        // and on into a `mix` that blacks out the sky. Asking whether it is inside the range instead
        // of whether it is outside is the whole of the difference.
        const float mixed = blend > 0.0f ? (blend < 1.0f ? blend : 1.0f) : 0.0f;

        // **The level the sheet is read against crosses with the sheet, and falls back the way it
        // does.** Where the weather ahead names no deck the shader samples the near sheet for both
        // ends of the blend, so what it read is that sheet alone and so is the mean it is read
        // against.
        const std::uint32_t ahead = textures.cloudsOf(next);
        const auto crossing = [&](float from, float to) {
            return ahead == Shaders::NO_TEXTURE ? from : from * (1.0f - mixed) + to * mixed;
        };

        const float mean = crossing(textures.meanOf(weather), textures.meanOf(next));
        const float cover = crossing(textures.coverOf(weather), textures.coverOf(next));

        return Shaders::CloudDeck{
            // A weather whose deck was never loaded has no deck, and neither has a sky whose mesh
            // gave up no shape to hang one on — which is the same thing an interior has, said the
            // same way.
            .mOpacity = slot == Shaders::NO_TEXTURE || !(textures.mShell.mTiles.x() > 0.0f) ? 0.0f : 1.0f,

            .mLit = light.mLit,
            .mShadowed = light.mShadowed,
            .mMean = mean,
            .mCover = cover,

            // **A world height and a tile's own width**, which is what anchors the sheet to the
            // ground under it rather than to the eye. `Rtx::sCloudAltitude` is the chosen number and
            // the mesh's own height in tiles is what turns it into a width.
            .mAltitude = sCloudAltitude,
            .mPerTile = textures.mShell.mTiles / sCloudAltitude,

            .mBlend = mixed,
            .mScroll = scroll,

            // **Turned to face where each weather is driving**, which is what the engine does to
            // each of its two cloud meshes: the deck of an ashstorm runs the way the ash does. A
            // weather with nothing to drive leaves the direction due north, and this due north too.
            .mBearing = bearingOf(storm),
            .mNextBearing = bearingOf(nextStorm),

            .mCurvature = textures.mShell.mCurvature,
            .mRings = textures.mShell.mRings,

            .mTexture = slot,
            .mNext = ahead,
        };
    }

    Shaders::StarField describeStars(float fade, float glare, float turn, const SkyContent& textures)
    {
        const float seen = fade * glare;

        const bool drawn = seen > 0.0f && textures.mNight.mField != sNoIndex && textures.mNight.mTile > 0.0f;

        return Shaders::StarField{
            .mFade = seen,
            .mGlow = textures.mNight.mGlow * seen,
            .mTurn = turn,
            .mTile = textures.mNight.mTile,
            .mHorizon = textures.mNight.mHorizon,

            // A sheet nobody can see is one nothing has to sample, and saying so here is what keeps
            // the test out of the shader's hot path.
            .mTexture = drawn ? static_cast<std::uint32_t>(textures.mNight.mField) : Shaders::NO_TEXTURE,
        };
    }

    void describePatches(
        float turn, const SkyContent& textures, std::span<Shaders::SkyPatch, Shaders::SKY_PATCH_COUNT> patches)
    {
        // Straight up with no texture, which is a patch the sky skips — and what an interior and a
        // mesh with fewer than six of them both leave behind.
        const Shaders::SkyPatch none = noPatch();

        for (std::size_t patch = 0; patch < patches.size(); ++patch)
        {
            const NightSky::Patch& placed = textures.mNight.mPatches[patch];
            if (placed.mTexture == sNoIndex || !(placed.mAngularRadius > 0.0f))
            {
                patches[patch] = none;
                continue;
            }

            // Turned with the star sphere, because that is the mesh they are painted on: a rotation
            // about the zenith, which is what the engine gives the whole night node.
            const osg::Vec3f towards(placed.mDirection.x() * std::cos(turn) - placed.mDirection.y() * std::sin(turn),
                placed.mDirection.x() * std::sin(turn) + placed.mDirection.y() * std::cos(turn), placed.mDirection.z());

            // **A canonical orientation, because the mesh's own is not recoverable from a centre and
            // a radius.** What a patch is painted with is a soft wash or a scatter of stars, neither
            // of which reads as turned the wrong way; keeping `mUp` as near the zenith as the patch
            // allows is what stops one drifting as the sphere rolls.
            osg::Vec3f up = osg::Vec3f(0.0f, 0.0f, 1.0f) - towards * towards.z();
            if (up.length2() < 1.0e-6f)
                up = osg::Vec3f(0.0f, 1.0f, 0.0f);
            up.normalize();

            osg::Vec3f right = up ^ towards;
            right.normalize();

            patches[patch] = Shaders::SkyPatch{ .mDirection = towards,
                .mRight = right,
                .mUp = up,
                .mAngularRadius = placed.mAngularRadius,
                .mTexture = static_cast<std::uint32_t>(placed.mTexture) };
        }
    }
}
