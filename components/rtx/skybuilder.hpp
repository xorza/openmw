#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <osg/Vec3f>

#include "cloudshell.hpp"
#include "nightsky.hpp"
#include "scenedesc.hpp"
#include "shaders/visibility.h"

namespace Rtx
{
    /// Everything the sky was read from the content files: its sheets, the surfaces they are laid
    /// on, and what a weather says a cloud is worth.
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

        /// How much brighter than the air a weather's cloud is, per channel and linear.
        ///
        /// **The engine's lift, read as the ratio it stands for.** `Sky::cloudColour` adds an eighth
        /// to a display-encoded fog, which in light is a multiplication that grows as the base
        /// darkens — a third again over a clear day and eight times over the same weather's night.
        /// The rasterizer never exposes a frame and so never notices; this one does, and a deck eight
        /// times its own sky is the grey lid a Morrowind night used to have. `Sky::dayFog` says why
        /// daylight is where the ratio is read.
        std::array<osg::Vec3f, Shaders::WEATHER_COUNT> mLift{};

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

        /// The night sky, read off the mesh the rasterizer draws it with: the star field, the scale
        /// its sheet is laid at, where it fades, and the six patches painted across it.
        NightSky mNight;

        /// The surface every weather's deck hangs on, read off the cloud mesh. One shell for all
        /// ten, because the engine draws all ten on the one mesh.
        CloudShell mShell;

        /// What the shader takes for a weather, or `NO_TEXTURE`.
        std::uint32_t cloudsOf(std::uint32_t weather) const;

        /// What lifts that weather's deck above its air, or no lift at all where none was read.
        osg::Vec3f liftOf(std::uint32_t weather) const;

        /// What that weather's sheet averages, or nothing where none was read or none could be.
        float meanOf(std::uint32_t weather) const;
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

    /// The cloud deck, in the units the shader takes.
    ///
    /// **One conversion and two callers.** The game reports what its weather system settled on and
    /// the harness derives the same numbers from the content files at an hour it was told; what a
    /// deck *is* once they are known lives here, so a screenshot and the game stand under one sky.
    ///
    /// @param air the weather's fog colour at this hour, linear — `Daylight::mSkyHorizon`. The
    ///        deck is this times `SkyContent::mLift`, multiplied here rather than in the shader
    ///        because the ratio belongs to a weather and the shader is handed one deck.
    /// @param storm where the weather drives what it carries, which is what the deck is turned by.
    /// @param scroll `Sky::SkyRoll::mClouds`.
    Shaders::CloudDeck describeClouds(std::uint32_t weather, std::uint32_t next, float blend, const osg::Vec3f& air,
        const osg::Vec3f& storm, float scroll, const SkyContent& textures);

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
