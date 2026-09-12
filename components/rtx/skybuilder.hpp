#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <osg/Vec3f>

#include <components/vfs/pathutil.hpp>

#include "cloudshell.hpp"
#include "moonbuilder.hpp"
#include "nightsky.hpp"
#include "runs.hpp"
#include "shaders/look.h"
#include "shaders/visibility.h"
#include "skylight.hpp"

namespace Rtx
{
    class SceneDesc;

    /// Which meshes the sky's two surfaces are read off — `Models/skyclouds` and the two star
    /// domes — named by the host, because this library holds no settings registry.
    struct SkyMeshes
    {
        /// The cap the cloud deck is painted on.
        VFS::Path::Normalized mClouds;

        /// The star dome, and the one to fall back to where the archives hold no `mStars`.
        VFS::Path::Normalized mStars;
        VFS::Path::Normalized mStarsFallback;
    };

    /// Everything the sky was read from the content files: its sheets, what each of them averages,
    /// and the surfaces they are laid on. The textures are held rather than named by a material,
    /// because they are found by rays that reached nothing and the sweep would take their slots
    /// back. All ten weathers at once, under a megabyte, so a storm arriving costs no upload.
    struct SkyContent
    {
        /// One per weather, in `WEATHER_*` order. `sNoIndex` where the content files record no
        /// cloud texture for that weather, which the shipped fallbacks do for ash and blight.
        std::array<Index, Shaders::WEATHER_COUNT> mClouds{};

        /// The mean luminance of what each weather's sheet paints, linear. Nought where no sheet
        /// was read. What a texel is read as a ratio to, so the painting gives shape and not a
        /// level: each sheet is a photograph of a 2002 sky with that day's light already in it. For
        /// half the decks it is the only shape there is — `tx_sky_overcast`, `_rainy` and
        /// `_thunder` carry an alpha of 255 in every texel; their means are 0.268, 0.283 and 0.357,
        /// against clear 0.435, cloudy 0.552, foggy 0.639. Measured over the alpha, because clear
        /// weather's cirrus covers a quarter of its own sheet.
        std::array<float, Shaders::WEATHER_COUNT> mCloudMean{};

        /// The mean alpha of each weather's sheet: how much sky its deck hides on average, which a
        /// cloud's shadow is measured against because the content's own `Sun_*_Color` has already
        /// dimmed the sun for that weather (`Shaders::CLOUD_SHADOW_DEPTH`). A quarter for clear
        /// weather's cirrus, three quarters for cloudy, all of it for the three opaque sheets.
        std::array<float, Shaders::WEATHER_COUNT> mCloudCover{};

        /// The night sky, read off the mesh the rasterizer draws it with: the star field, the scale
        /// its sheet is laid at, where it fades, and the six patches painted across it.
        NightSky mNight;

        /// The surface every weather's deck hangs on, read off the cloud mesh. One shell for all
        /// ten, because the engine draws all ten on the one mesh.
        CloudShell mShell;

        /// What the shader takes for a weather, or `NO_TEXTURE`.
        std::uint32_t cloudsOf(std::uint32_t weather) const;

        /// What that weather's sheet averages, or nothing where none was read or none could be.
        float meanOf(std::uint32_t weather) const;

        /// How much sky that weather's deck hides on average, or nothing where none was read.
        float coverOf(std::uint32_t weather) const;
    };

    /// Reads all of it, loading the textures into `scene` and holding them there for the life of
    /// the scene. A texture the archives do not hold is left out rather than reserved: the shipped
    /// fallbacks name Solstheim's two skies without Bloodmoon, and the unreadable stand-in is an
    /// opaque grey, which over a cloud deck is the entire sky.
    SkyContent addSkyContent(SceneDesc& scene, Resource::SceneManager& scenes, const SkyMeshes& meshes);

    /// What a cloud deck radiates from below, where its own body shadows it and where it does not.
    /// The deck takes only the *shape* out of a sheet (`SkyContent::mCloudMean`) and the colour
    /// comes from here.
    struct DeckLight
    {
        /// What a cloud in full sunlight shows: everything reaching the top of the layer.
        osg::Vec3f mLit;

        /// What a cloud in its own shadow shows: the sky alone, which is the light a deck cannot
        /// keep off its own base.
        osg::Vec3f mShadowed;
    };

    /// Lights a deck by what stands over it, however a sun, a sky and two moons were reached.
    ///
    /// @param skyMean what the sky over the deck delivers, as a radiance — `SkyBudget::mMean`.
    /// @param moons both of them, whether or not either is up: a moon that is down delivers nothing
    ///        and needs no test of its own.
    DeckLight deckLight(const Sun& sun, const osg::Vec3f& skyMean, std::span<const MoonPlacement, 2> moons);

    /// Which weather's deck is over the eye, which one is arriving, and how the two sheets stand.
    struct CloudCrossing
    {
        std::uint32_t mWeather = Shaders::WEATHER_CLEAR;
        std::uint32_t mNext = Shaders::WEATHER_CLEAR;

        /// How far the deck has crossed from this weather's sheet to the next one's.
        float mBlend = 0.0f;

        /// Where each weather drives what it carries, which is what its sheet is turned by. The
        /// engine turns each of its two cloud meshes by its own weather's storm; a direction nobody
        /// stated reads as due north rather than as a sheet with no size.
        osg::Vec3f mDirection = osg::Vec3f(0.0f, 1.0f, 0.0f);
        osg::Vec3f mNextDirection = osg::Vec3f(0.0f, 1.0f, 0.0f);

        /// `MWRender::WorldState::mCloudScroll`, which both sheets share: the engine sets one texture matrix on
        /// both of its cloud updaters.
        float mScroll = 0.0f;
    };

    /// The cloud deck, in the units the shader takes — one conversion, so a screenshot and a
    /// played frame stand under one sky.
    ///
    /// @param light what the deck radiates, out of `deckLight`.
    Shaders::CloudDeck describeClouds(const CloudCrossing& clouds, const DeckLight& light, const SkyContent& textures);

    /// The star field, in the units the shader takes.
    ///
    /// @param fade the engine's `Stars` ramp at this hour, which is what brings them out at dusk.
    /// @param glare the weather's `Glare_View`, which is what keeps them in under an overcast.
    /// @param turn `MWRender::WorldState::mStarRoll`.
    Shaders::StarField describeStars(float fade, float glare, float turn, const SkyContent& textures);

    /// The nebulae and the constellations, placed — the same shape a moon is, a direction, a size
    /// and a texture, drawn as the disc the moons are. Where they go was read off the mesh.
    ///
    /// @param turn `MWRender::WorldState::mStarRoll`, because they are on the star sphere.
    /// @param patches written here rather than returned, so a frame's description costs no
    ///        allocation.
    void describePatches(
        float turn, const SkyContent& textures, std::span<Shaders::SkyPatch, Shaders::SKY_PATCH_COUNT> patches);
}
