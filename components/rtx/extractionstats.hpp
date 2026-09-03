#pragma once

#include <array>
#include <cstdint>

#include "imageformat.hpp"

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

    /// What one extraction pass did.
    ///
    /// The reused counts are the interesting half: a mirror that adds nothing on a second pass over
    /// an unchanged graph is the property the whole incremental design rests on, and it is only
    /// visible as a number.
    struct ExtractionStats
    {
        /// Distinct geometry met for the first time, so one new entry in the scene each.
        std::uint32_t mMeshesAdded = 0;
        std::uint32_t mMaterialsAdded = 0;

        /// Of those, the meshes that were nothing but reversed pairs and left here as one copy
        /// each. `ShapeFold` says what a sheet is; a cell with foliage in it has hundreds.
        std::uint32_t mSheets = 0;

        /// Ground wide enough that its layer stack is baked into one texture rather than shaded a
        /// layer at a time. Distant chunks and nothing else — see `sCompositeFrom`.
        std::uint32_t mComposites = 0;

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

        /// Skinned drawables mirrored as they stand, because no update traversal has resolved their
        /// skeleton: `SceneUtil::RigGeometry::getBones` answered nothing. The rasterizer draws such a
        /// rig in its bind pose too, so this is what it shows and not a loss — but a walk that
        /// reaches a rig before the update that should have found its skeleton is a walk out of
        /// order, and this is the number that says so.
        std::uint32_t mUnskinned = 0;

        /// Particle systems met, and the live particles they were holding.
        ///
        /// **Sprites and not triangles**, so neither number is a mesh or an instance: an emitter is
        /// a sphere and a run of discs the primary ray composites, and nothing about it reaches an
        /// acceleration structure. An emitter whose particles have all died places nothing and is
        /// not counted.
        std::uint32_t mEmitters = 0;
        std::uint32_t mSprites = 0;

        /// Drawables this cannot read at all — neither an `osg::Geometry`, nor either of the two
        /// deforming kinds, nor a particle system.
        ///
        /// What is left is OpenMW's own debug drawing, which a ray tracer answers differently
        /// rather than misses. A canary and not a deficit: what it would catch is a new kind of
        /// drawable arriving unnoticed.
        std::uint32_t mSkippedUnknown = 0;

        /// Surfaces the content pipeline never described, which are drawn as whatever a default
        /// `Material` is — untextured, opaque and one-sided.
        ///
        /// **A canary, and it should be zero.** `NifOsg` and `Terrain` author a `Surface::Material`
        /// for everything they build; a drawable arriving without one means a state set was made
        /// somewhere else, or remade by something that copied the pipeline state and dropped the
        /// description with it.
        std::uint32_t mUndescribedMaterials = 0;

        /// What the textures a scene reached for turned out to be, one entry per `ImageFormat`.
        ///
        /// Kept because the answer decides how they are uploaded, and guessing it from what the
        /// content files ought to contain is how a renderer ends up with a path nothing takes.
        ///
        /// **Counted by enumerator and named at the end.** A walk meets every texture of every
        /// material it reads, and animated ones again on each frame; naming one where it is met
        /// builds a `std::string` on the frame path to key a map by.
        std::array<FormatCount, sImageFormatCount> mTextureFormats{};

        /// The pixel format the `Unnamed` count last stood for, or zero.
        ///
        /// The number is the whole of what makes that count worth printing: a format nothing names
        /// is a canary, and the reader's next step is to look this one up.
        std::uint32_t mUnnamedFormat = 0;

        /// Geometry with no vertices or no triangles. Morrowind ships some.
        std::uint32_t mSkippedEmpty = 0;

        /// `LightSource`s taken off the graph, which is every lamp the scene has: a `LIGH` record is
        /// what Morrowind lights with, and a glowing texture lights nothing.
        std::uint32_t mLights = 0;

        /// Placements of a mesh whose material is a cutout that a controller rewrites every frame,
        /// which is a mask no backend can bake a micromap against — a scrolling banner, a flipbook
        /// of leaves. Each is an instance traversal still has to stop and ask about.
        std::uint32_t mUnbakeable = 0;

        /// Placements wearing a material other than the one their mesh arrived with, where that one
        /// is not animated.
        ///
        /// **A canary, and it should be zero.** `MeshRange::mMaterial` says why a static mesh wears
        /// one material by construction; a backend bakes against that one, and a placement wearing
        /// another would be traced against a mask it does not carry. The loader says it cannot
        /// happen, and this is the number that says so every frame.
        std::uint32_t mWornOtherwise = 0;

        ExtractionStats& operator+=(const ExtractionStats& other);
    };

    /// What one sweep dropped.
    struct Retirement
    {
        std::uint32_t mMeshes = 0;
        std::uint32_t mMaterials = 0;

        bool empty() const { return mMeshes == 0 && mMaterials == 0; }
    };

    /// Counts `image` under its format, and its mips beside it.
    void countFormat(const osg::Image& image, ExtractionStats& stats);
}
