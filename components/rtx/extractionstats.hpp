#pragma once

#include <array>
#include <cstdint>

#include "texels.hpp"

namespace osg
{
    class Image;
}

namespace Rtx
{
    /// How many textures of one format a walk met, and how many of those brought mips.
    struct FormatCount
    {
        std::uint32_t mMet = 0;
        std::uint32_t mMipped = 0;
    };

    /// What the textures a walk reached for turned out to be, one entry per `ImageFormat`, counted
    /// by enumerator and named at the end, because naming one where it is met builds a
    /// `std::string` on the frame path. Its own struct because the unnamed format is the last one
    /// seen rather than a total.
    struct FormatCensus
    {
        std::array<FormatCount, sImageFormatCount> mMet{};

        /// The pixel format the `Unnamed` count last stood for, or zero — the whole of what makes
        /// that count worth printing, because a format nothing names is a canary and the reader's
        /// next step is to look this one up.
        std::uint32_t mUnnamed = 0;

        /// Counts `image` under its format, and its mips beside it.
        void count(const osg::Image& image);

        FormatCensus& operator+=(const FormatCensus& other);
    };

    /// What one extraction pass did. The reused counts are the interesting half: a mirror that
    /// adds nothing on a second pass over an unchanged graph is only visible as a number.
    struct ExtractionStats
    {
        /// Distinct geometry met for the first time, so one new entry in the scene each.
        std::uint32_t mMeshesAdded = 0;
        std::uint32_t mMaterialsAdded = 0;

        /// What the folding cost, of the meshes added above. Timed rather than counted, because
        /// what it costs is triangles and not drawables: one entry of `mMeshesAdded` can be a
        /// building and its neighbour a crate.
        double mFoldMs = 0.0;

        /// Of those, the meshes that were nothing but reversed pairs and left here as one copy
        /// each. `ShapeFold` says what a sheet is; a cell with foliage in it has hundreds.
        std::uint32_t mSheets = 0;

        /// Drawables that resolved to something already known. A count of lookups, not of meshes:
        /// a hundred crates sharing one model contribute a hundred here and one above.
        std::uint32_t mMeshesReused = 0;
        std::uint32_t mMaterialsReused = 0;
        std::uint32_t mInstances = 0;

        /// Drawables whose vertices are recomputed every frame and so were posed rather than read
        /// from the cache: skinned bodies and morphed faces. Each one already met is a dispatch and
        /// a bottom-level structure a backend has to refit, which is what makes this the cost of an
        /// actor rather than a count of them.
        std::uint32_t mDeformed = 0;

        /// Skinned drawables mirrored as they stand, because `SceneUtil::RigGeometry::getBones`
        /// answered nothing. The rasterizer draws such a rig in its bind pose too; the number says
        /// a walk reached a rig before the update that should have found its skeleton.
        std::uint32_t mUnskinned = 0;

        /// Particle systems met, and the live particles they were holding — sprites and not
        /// triangles, so neither number is a mesh or an instance. An emitter whose particles have
        /// all died is not counted.
        std::uint32_t mEmitters = 0;
        std::uint32_t mSprites = 0;

        /// Drawables this cannot read at all, which is OpenMW's own debug drawing. A canary for a
        /// new kind of drawable arriving unnoticed.
        std::uint32_t mSkippedUnknown = 0;

        /// Surfaces the content pipeline never described, drawn as a default `Material` —
        /// untextured, opaque and one-sided. A canary that should be zero: `NifOsg` describes
        /// everything it builds.
        std::uint32_t mUndescribedSurfaces = 0;

        /// Particle systems the walk met and could not draw, because nothing described them or what
        /// did named no diffuse map. One of these is the rasterizer's `MWRender::RippleSimulation`,
        /// built by hand under `Mask_Water` in every world, and the traced path draws no ripple
        /// sprites (`VisibilityConstants::mRainOnWater`), so it is a canary and not a deficit. Three
        /// counts and not one, because an undescribed surface, an undescribed ground pass and a
        /// missing plume each cost something different.
        std::uint32_t mSpritelessEmitters = 0;

        FormatCensus mFormats;

        /// Geometry with no vertices or no triangles. Morrowind ships some.
        std::uint32_t mSkippedEmpty = 0;

        /// `LightSource`s taken off the graph, which is every lamp the scene has: a `LIGH` record is
        /// what Morrowind lights with, and a glowing texture lights nothing.
        std::uint32_t mLights = 0;

        /// Placements wearing a material other than the one their mesh arrived with, where that one
        /// is not animated. A canary that should be zero: a backend bakes against the mesh's
        /// material, and a placement wearing another would be traced against a mask it does not
        /// carry.
        std::uint32_t mWornOtherwise = 0;

        /// Placements the cell ring stood this walk: the distant statics, as instances of their
        /// templates rather than as the paging's merged chunks, and the cells' ground, one
        /// placement a cell. Among `mInstances` as well.
        std::uint32_t mDistantStatics = 0;
        std::uint32_t mGroundCells = 0;

        ExtractionStats& operator+=(const ExtractionStats& other);
    };

    /// What one sweep dropped.
    struct Retirement
    {
        std::uint32_t mMeshes = 0;
        std::uint32_t mMaterials = 0;

        bool empty() const { return mMeshes == 0 && mMaterials == 0; }
    };

}
