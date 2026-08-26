#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include "nightsky.hpp"
#include "scenedesc.hpp"
#include "shaders/visibility.h"

namespace Rtx
{
    /// The sky's own textures, in a scene's texture table.
    ///
    /// **Held rather than named by a material**, for the reason the moons' faces are: the deck and
    /// the star sheet are found by rays that reached nothing, so no material can speak for them and
    /// the sweep would take their slots back on the first frame a cell died.
    ///
    /// All ten weathers at once rather than the two a frame needs. A transition runs between two of
    /// them and a player can walk into a region that offers neither, so loading on demand would put
    /// a texture upload on the frame a storm arrives — which is the one frame that can least afford
    /// it. Ten sky textures is under a megabyte.
    struct SkyTextures
    {
        /// One per weather, in `WEATHER_*` order. `sNoIndex` where the content files record no
        /// cloud texture for that weather, which the shipped fallbacks do for ash and blight.
        std::array<Index, Shaders::WEATHER_COUNT> mClouds{};

        /// The night sky, read off the mesh the rasterizer draws it with: the star field, the scale
        /// its sheet is laid at, where it fades, and the six patches painted across it.
        NightSky mNight;

        /// What the shader takes for a weather, or `NO_TEXTURE`.
        std::uint32_t cloudsOf(std::uint32_t weather) const;
    };

    /// Loads the sky's textures into `scene` and holds them there.
    ///
    /// **A texture the archives do not hold is left out rather than reserved**, which is what `vfs`
    /// is for. The shipped fallbacks name Solstheim's two skies without Bloodmoon's `bm` in them, so
    /// a slot taken for either would be filled by the unreadable stand-in — and the stand-in is an
    /// opaque grey, which over a cloud deck is the entire sky. Missing means no deck, as it does for
    /// the two weathers that name no texture at all.
    ///
    /// Safe to call again on a scene that already has them — `addTexture` hands back the slot it
    /// already gave — though each call takes a hold, so each wants its own `dropSkyTextures`.
    SkyTextures addSkyTextures(SceneDesc& scene, Resource::SceneManager& scenes);

    /// Gives back the holds `addSkyTextures` took.
    void dropSkyTextures(SceneDesc& scene, const SkyTextures& textures);

    /// The cloud deck, in the units the shader takes.
    ///
    /// **One conversion and two callers.** The game reports what its weather system settled on and
    /// the harness derives the same numbers from the content files at an hour it was told; what a
    /// deck *is* once they are known lives here, so a screenshot and the game stand under one sky.
    ///
    /// @param fog the weather's fog colour, in the space the file records it. `Sky::cloudColour` is
    ///        what lifts it and this is what decodes the result, in that order and for that reason.
    /// @param storm where the weather drives what it carries, which is what the deck is turned by.
    /// @param scroll `Sky::SkyRoll::mClouds`.
    Shaders::CloudDeck describeClouds(std::uint32_t weather, std::uint32_t next, float blend, const osg::Vec4f& fog,
        const osg::Vec3f& storm, float scroll, const SkyTextures& textures);

    /// The star field, in the units the shader takes.
    ///
    /// @param fade the engine's `Stars` ramp at this hour, which is what brings them out at dusk.
    /// @param glare the weather's `Glare_View`, which is what keeps them in under an overcast.
    /// @param turn `Sky::SkyRoll::mStars`.
    Shaders::StarField describeStars(float fade, float glare, float turn, const SkyTextures& textures);

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
        float turn, const SkyTextures& textures, std::span<Shaders::SkyPatch, Shaders::SKY_PATCH_COUNT> patches);
}
