#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <osg/Vec3f>

#include "cloudshell.hpp"
#include "lightbuilder.hpp"
#include "moonbuilder.hpp"
#include "nightsky.hpp"
#include "scenedesc.hpp"
#include "shaders/visibility.h"

namespace Rtx
{
    /// Everything the sky was read from the content files: its sheets, what each of them averages,
    /// and the surfaces they are laid on.
    ///
    /// **The textures are held rather than named by a material**, for the reason the moons' faces
    /// are: the deck and the star sheet are found by rays that reached nothing, so no material can
    /// speak for them and the sweep would take their slots back on the first frame a cell died.
    ///
    /// All ten weathers at once rather than the two a frame needs. A transition runs between two of
    /// them and a player can walk into a region that offers neither, so loading on demand would put
    /// a texture upload on the frame a storm arrives — which is the one frame that can least afford
    /// it. Ten sky textures is under a megabyte.
    struct SkyContent
    {
        /// One per weather, in `WEATHER_*` order. `sNoIndex` where the content files record no
        /// cloud texture for that weather, which the shipped fallbacks do for ash and blight.
        std::array<Index, Shaders::WEATHER_COUNT> mClouds{};

        /// The mean luminance of what each weather's sheet paints, linear. Nought where no sheet
        /// was read, which is a weather that draws no deck.
        ///
        /// **What a texel is read as a ratio to, so the painting gives shape and not a level.** Each
        /// sheet is a photograph of a 2002 sky with that day's light already in it, so compositing
        /// one lights every cloud twice. Against its own mean it carries where the cloud is thick
        /// and where it is thin and nothing else, which is the half of it a lit deck can use.
        ///
        /// **And for half the decks it is the only shape there is.** Six of the ten weathers reach a
        /// sheet the archives hold, and `tx_sky_overcast`, `_rainy` and `_thunder` carry an alpha of
        /// 255 in every one of their texels — so a deck cut out of the alpha alone is a flat lid
        /// across the whole sky. Their means are 0.268, 0.283 and 0.357, against the three that do
        /// carry an alpha: clear 0.435, cloudy 0.552, foggy 0.639.
        ///
        /// Measured over the alpha rather than over the whole sheet, because clear weather's cirrus
        /// covers a quarter of its own sheet and its wisps are not a quarter as bright as they look.
        std::array<float, Shaders::WEATHER_COUNT> mCloudMean{};

        /// The mean alpha of each weather's sheet: how much sky its deck hides on average.
        ///
        /// **What a cloud's shadow is measured against.** `Shaders::CLOUD_SHADOW_DEPTH` says why the
        /// average cloud must darken nothing — the content's own `Sun_*_Color` has already dimmed
        /// the sun for that weather, and a shadow that darkened by the whole of the alpha would
        /// state it twice.
        ///
        /// A quarter for clear weather's cirrus, three quarters for cloudy, and all of it for the
        /// three sheets that are 255 alpha in every texel.
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

    /// Reads all of it, loading the textures into `scene` and holding them there.
    ///
    /// **A texture the archives do not hold is left out rather than reserved**, which is what `vfs`
    /// is for. The shipped fallbacks name Solstheim's two skies without Bloodmoon's `bm` in them, so
    /// a slot taken for either would be filled by the unreadable stand-in — and the stand-in is an
    /// opaque grey, which over a cloud deck is the entire sky. Missing means no deck, as it does for
    /// the two weathers that name no texture at all.
    ///
    /// Safe to call again on a scene that already has them — `addTexture` hands back the slot it
    /// already gave — though each call takes a hold, so each wants its own `dropSkyContent`.
    SkyContent addSkyContent(SceneDesc& scene, Resource::SceneManager& scenes);

    /// Gives back the holds `addSkyContent` took.
    void dropSkyContent(SceneDesc& scene, const SkyContent& textures);

    /// What a cloud deck radiates from below, where its own body shadows it and where it does not.
    ///
    /// **A vanilla asset lit rather than shown.** Every sky sheet is a photograph of a 2002 sky with
    /// that day's light already painted into it, so the deck takes only the *shape* out of one and
    /// the colour comes from here. `SkyContent::mCloudMean` carries the other half of that split.
    struct DeckLight
    {
        /// What a cloud in full sunlight shows: everything reaching the top of the layer.
        osg::Vec3f mLit;

        /// What a cloud in its own shadow shows: the sky alone, which is the light a deck cannot
        /// keep off its own base.
        osg::Vec3f mShadowed;
    };

    /// Lights a deck by what stands over it.
    ///
    /// **One place, because a deck is lit like anything else and this is the only thing that knows
    /// it.** The game and the harness each reach a sun, a sky and two moons their own way; what a
    /// layer of water droplets does with the three is the same either way.
    ///
    /// @param skyMean what the sky over the deck delivers, as a radiance — `SkyBudget::mMean`.
    /// @param moons both of them, whether or not either is up: a moon that is down delivers nothing
    ///        and needs no test of its own.
    DeckLight deckLight(const Sun& sun, const osg::Vec3f& skyMean, std::span<const MoonPlacement, 2> moons);

    /// The cloud deck, in the units the shader takes.
    ///
    /// **One conversion and two callers.** The game reports what its weather system settled on and
    /// the harness derives the same numbers from the content files at an hour it was told; what a
    /// deck *is* once they are known lives here, so a screenshot and the game stand under one sky.
    ///
    /// @param light what the deck radiates, out of `deckLight` — worked out on the host rather than
    ///        in the shader because it is one answer for the whole frame.
    /// @param storm where this weather drives what it carries, which is what its sheet is turned by.
    /// @param nextStorm the same for the weather ahead, because the engine turns each of its two
    ///        cloud meshes by its own weather's storm. A settled sky has no sheet ahead to turn, and
    ///        a direction nobody stated reads as due north rather than as a sheet with no size.
    /// @param scroll `Sky::SkyRoll::mClouds`, which both sheets share — the engine sets one texture
    ///        matrix on both of its cloud updaters.
    Shaders::CloudDeck describeClouds(std::uint32_t weather, std::uint32_t next, float blend, const DeckLight& light,
        const osg::Vec3f& storm, const osg::Vec3f& nextStorm, float scroll, const SkyContent& textures);

    /// The star field, in the units the shader takes.
    ///
    /// @param fade the engine's `Stars` ramp at this hour, which is what brings them out at dusk.
    /// @param glare the weather's `Glare_View`, which is what keeps them in under an overcast.
    /// @param turn `Sky::SkyRoll::mStars`.
    Shaders::StarField describeStars(float fade, float glare, float turn, const SkyContent& textures);

    /// The nebulae and the constellations, placed.
    ///
    /// **The same shape a moon is**, which is the point: each is a sheet laid once across a patch of
    /// sky, so what it comes to is a direction, a size and a texture — and the disc the moons are
    /// already drawn as is what draws it. Where they go was read off the mesh, not written down.
    ///
    /// @param turn `Sky::SkyRoll::mStars`, because they are on the star sphere and turn with it.
    /// @param patches written here rather than returned, so a frame's description costs no
    ///        allocation.
    void describePatches(
        float turn, const SkyContent& textures, std::span<Shaders::SkyPatch, Shaders::SKY_PATCH_COUNT> patches);
}
