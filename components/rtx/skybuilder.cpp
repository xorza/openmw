#include "skybuilder.hpp"

#include <cmath>
#include <string>

#include <osg/Image>

#include <components/misc/strings/lower.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sky/clouds.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "lightbuilder.hpp"
#include "meantexel.hpp"
#include "shaders/colour.h"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// What a weather's own daylight says a cloud is worth against the air it hangs in.
        ///
        /// **Daylight, because that is the hour the engine's lift means what it says.** An eighth
        /// added to a display-encoded colour is a multiplication in light, and how large a one
        /// depends on the base: a clear day's fog is lifted by 1.40, 1.36 and 1.32 across the three
        /// channels, the same weather's night by eight. The bright end is where the offset is small
        /// against what it is added to and so behaves like the ratio a cloud actually is — a sheet
        /// of droplets scattering back some fixed share more of the light than the clear air beside
        /// it. Read there and applied everywhere, a day's deck comes out exactly where the engine
        /// puts it and a night's follows its own sky down.
        osg::Vec3f cloudLift(std::string_view weather)
        {
            const osg::Vec4f day = Sky::dayFog(weather);
            const osg::Vec3f air = decodeColour(day);
            const osg::Vec3f lit = decodeColour(Sky::cloudColour(day));

            // **A channel a weather records as black holds no ratio**, and dividing by it is a
            // division by however near nothing the decode came. One code value is the least a
            // content file can write and still mean a colour, so under that the lift is not read.
            const auto worth = [](float over, float under, float recorded) {
                return recorded >= 1.0f / 255.0f ? over / under : 1.0f;
            };

            return osg::Vec3f(
                worth(lit.x(), air.x(), day.x()), worth(lit.y(), air.y(), day.y()), worth(lit.z(), air.z(), day.z()));
        }

        /// The mean luminance of what a sheet paints, which `SkyContent::mCloudMean` is the table of.
        ///
        /// Nothing where the file will not read or decode, which is the same answer a missing sheet
        /// gives and lands in the same place: a weather with no deck to draw.
        float sheetMean(Resource::ImageManager& images, const VFS::Path::Normalized& path)
        {
            const osg::ref_ptr<const osg::Image> image = openImage(images, path);
            if (image == nullptr)
                return 0.0f;

            return meanTexel(*image).opaque() * Shaders::LUMINANCE_WEIGHTS;
        }
    }

    std::uint32_t SkyContent::cloudsOf(std::uint32_t weather) const
    {
        if (weather >= mClouds.size() || mClouds[weather] == sNoIndex)
            return Shaders::NO_TEXTURE;

        return static_cast<std::uint32_t>(mClouds[weather]);
    }

    osg::Vec3f SkyContent::liftOf(std::uint32_t weather) const
    {
        // A lift of nothing is a deck of nothing, which no content says and no reading gives — so it
        // is what an unread table means, the way `sNoIndex` is for the textures beside it.
        if (weather >= mLift.size() || mLift[weather].length2() <= 0.0f)
            return osg::Vec3f(1.0f, 1.0f, 1.0f);

        return mLift[weather];
    }

    float SkyContent::meanOf(std::uint32_t weather) const
    {
        return weather < mCloudMean.size() ? mCloudMean[weather] : 0.0f;
    }

    SkyContent addSkyContent(SceneDesc& scene, Resource::SceneManager& scenes)
    {
        const VFS::Manager& vfs = *scenes.getVFS();

        SkyContent loaded;
        loaded.mClouds.fill(sNoIndex);

        for (std::uint32_t weather = 0; weather < Shaders::WEATHER_COUNT; ++weather)
        {
            const std::string_view named = weatherName(weather);
            loaded.mLift[weather] = cloudLift(named);

            const std::string_view sheet = Sky::cloudTexture(named);
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
            loaded.mCloudMean[weather] = sheetMean(*scenes.getImageManager(), path);
        }

        // **The shape the deck hangs on is the mesh's**, both of its numbers: how high the layer is
        // in tiles of its own sheet, and how far it falls away over the ground it covers.
        loaded.mShell = readCloudShell(scenes);

        // **The night sky is the mesh's**, every number of it: which sheet the field wears, how much
        // sky a tile of it covers, where it fades out, and where the six patches sit.
        loaded.mNight = readNightSky(scene, scenes);

        return loaded;
    }

    void dropSkyContent(SceneDesc& scene, const SkyContent& textures)
    {
        for (const Index slot : textures.mClouds)
            if (slot != sNoIndex)
                scene.dropTexture(slot);

        dropNightSky(scene, textures.mNight);
    }

    Shaders::CloudDeck describeClouds(std::uint32_t weather, std::uint32_t next, float blend, const osg::Vec3f& air,
        const osg::Vec3f& storm, float scroll, const SkyContent& textures)
    {
        const std::uint32_t slot = textures.cloudsOf(weather);

        // **Written so a NaN lands on nought, which `std::clamp` does not do.** The blend comes off a
        // content file by way of a division, and a content file is untrusted: `clamp` compares and
        // hands back what it was given when both comparisons fail, so a NaN goes straight through it
        // and on into a `mix` that blacks out the sky. Asking whether it is inside the range instead
        // of whether it is outside is the whole of the difference.
        const float mixed = blend > 0.0f ? (blend < 1.0f ? blend : 1.0f) : 0.0f;

        // **The lift crosses with the sheet it belongs to**, on the deck's own schedule rather than
        // the weather's: how much brighter than the air a cloud is belongs to the cloud, so it
        // arrives when the cloud does. The fog under it is already the hour's and already blended.
        const osg::Vec3f lift = textures.liftOf(weather) * (1.0f - mixed) + textures.liftOf(next) * mixed;

        // **The level the sheet is read against crosses with the sheet, and falls back the way it
        // does.** Where the weather ahead names no deck the shader samples the near sheet for both
        // ends of the blend, so what it read is that sheet alone and so is the mean it is read
        // against.
        const std::uint32_t ahead = textures.cloudsOf(next);
        const float mean = ahead == Shaders::NO_TEXTURE
            ? textures.meanOf(weather)
            : textures.meanOf(weather) * (1.0f - mixed) + textures.meanOf(next) * mixed;

        return Shaders::CloudDeck{
            // A weather whose deck was never loaded has no deck, and neither has a sky whose mesh
            // gave up no shape to hang one on — which is the same thing an interior has, said the
            // same way.
            .mOpacity = slot == Shaders::NO_TEXTURE || !(textures.mShell.mTiles.x() > 0.0f) ? 0.0f : 1.0f,

            .mColour = osg::componentMultiply(air, lift),
            .mMean = mean,
            .mBlend = mixed,
            .mScroll = scroll,

            // **Turned to face where the weather is driving**, which is what the engine does to the
            // whole cloud mesh: the deck of an ashstorm runs the way the ash does. A weather with
            // nothing to drive leaves the direction due north, and this at nought.
            .mTurn = std::atan2(storm.x(), storm.y()),

            .mTiles = textures.mShell.mTiles,
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
        const Shaders::SkyPatch none{ .mDirection = osg::Vec3f(0.0f, 0.0f, 1.0f),
            .mRight = osg::Vec3f(1.0f, 0.0f, 0.0f),
            .mUp = osg::Vec3f(0.0f, 1.0f, 0.0f),
            .mAngularRadius = 0.0f,
            .mTexture = Shaders::NO_TEXTURE };

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
