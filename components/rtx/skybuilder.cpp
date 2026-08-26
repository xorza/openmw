#include "skybuilder.hpp"

#include <cmath>
#include <string>

#include <components/misc/strings/lower.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sky/clouds.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "lightbuilder.hpp"

namespace Rtx
{
    std::uint32_t SkyTextures::cloudsOf(std::uint32_t weather) const
    {
        if (weather >= mClouds.size() || mClouds[weather] == sNoIndex)
            return Shaders::NO_TEXTURE;

        return static_cast<std::uint32_t>(mClouds[weather]);
    }

    SkyTextures addSkyTextures(SceneDesc& scene, Resource::SceneManager& scenes)
    {
        const VFS::Manager& vfs = *scenes.getVFS();
        const auto hold = [&](const std::string& name) {
            // The file records a bare name and the archive holds it under `textures/`, which is the
            // same join the scene manager makes before it is handed one.
            const VFS::Path::Normalized path("textures/" + Misc::StringUtils::lowerCase(name));
            if (!vfs.exists(path))
                return sNoIndex;

            const Index slot = scene.addTexture(path);
            scene.holdTexture(slot);
            return slot;
        };

        SkyTextures loaded;
        loaded.mClouds.fill(sNoIndex);

        for (std::uint32_t weather = 0; weather < Shaders::WEATHER_COUNT; ++weather)
        {
            const std::string_view named = Sky::cloudTexture(weatherName(weather));
            if (!named.empty())
                loaded.mClouds[weather] = hold(std::string(named));
        }

        // **The night sky is the mesh's**, every number of it: which sheet the field wears, how much
        // sky a tile of it covers, where it fades out, and where the six patches sit.
        loaded.mNight = readNightSky(scene, scenes);

        return loaded;
    }

    void dropSkyTextures(SceneDesc& scene, const SkyTextures& textures)
    {
        for (const Index slot : textures.mClouds)
            if (slot != sNoIndex)
                scene.dropTexture(slot);

        dropNightSky(scene, textures.mNight);
    }

    Shaders::CloudDeck describeClouds(std::uint32_t weather, std::uint32_t next, float blend, const osg::Vec4f& fog,
        const osg::Vec3f& storm, float scroll, const SkyTextures& textures)
    {
        const std::uint32_t slot = textures.cloudsOf(weather);

        // **Written so a NaN lands on nought, which `std::clamp` does not do.** The blend comes off a
        // content file by way of a division, and a content file is untrusted: `clamp` compares and
        // hands back what it was given when both comparisons fail, so a NaN goes straight through it
        // and on into a `mix` that blacks out the sky. Asking whether it is inside the range instead
        // of whether it is outside is the whole of the difference.
        const float mixed = blend > 0.0f ? (blend < 1.0f ? blend : 1.0f) : 0.0f;

        return Shaders::CloudDeck{
            // A weather whose deck was never loaded has no deck, which is the same thing an interior
            // has and is said the same way.
            .mOpacity = slot == Shaders::NO_TEXTURE ? 0.0f : 1.0f,

            .mColour = decodeColour(Sky::cloudColour(fog)),
            .mBlend = mixed,
            .mScroll = scroll,

            // **Turned to face where the weather is driving**, which is what the engine does to the
            // whole cloud mesh: the deck of an ashstorm runs the way the ash does. A weather with
            // nothing to drive leaves the direction due north, and this at nought.
            .mTurn = std::atan2(storm.x(), storm.y()),

            .mTexture = slot,
            .mNext = textures.cloudsOf(next),
        };
    }

    Shaders::StarField describeStars(float fade, float glare, float turn, const SkyTextures& textures)
    {
        const float seen = fade * glare;

        const bool drawn = seen > 0.0f && textures.mNight.mField != sNoIndex && textures.mNight.mTile > 0.0f;

        return Shaders::StarField{
            .mFade = seen,
            .mTurn = turn,
            .mTile = textures.mNight.mTile,
            .mHorizon = textures.mNight.mHorizon,

            // A sheet nobody can see is one nothing has to sample, and saying so here is what keeps
            // the test out of the shader's hot path.
            .mTexture = drawn ? static_cast<std::uint32_t>(textures.mNight.mField) : Shaders::NO_TEXTURE,
        };
    }

    void describePatches(
        float turn, const SkyTextures& textures, std::span<Shaders::SkyPatch, Shaders::SKY_PATCH_COUNT> patches)
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
