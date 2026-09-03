#pragma once

#include <cstdint>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Vec2f>
#include <osg/Vec3f>

// For `RigGeometry::InfluenceData`, which is what a rig is keyed on: a nested type cannot be
// forward-declared. It brings `osg::Vec3Array`, which a morph is keyed on, with it.
#include <components/sceneutil/riggeometry.hpp>

#include "index.hpp"
#include "mirroridentity.hpp"
#include "scenedesc.hpp"
#include "shaders/skinning.h"
#include "shapefold.hpp"

namespace osg
{
    class Drawable;
    class Geometry;
}

namespace SceneUtil
{
    class MorphGeometry;
}

namespace Rtx
{
    struct ExtractionStats;

    /// Turns the drawables a walk met into the scene's meshes, and poses the ones that deform.
    ///
    /// **Keyed on the drawable and not on the geometry.** A crate met again is the crate already
    /// uploaded; a body met again is the same mesh posed again, and a pose is bone rows and never
    /// vertices. That identity is what makes an incremental mirror possible instead of a rebuild
    /// per frame, and it is why this holds state at all.
    ///
    /// **The rigs and the morphs are here because a mesh is what names one.** A skin is one
    /// `InfluenceData` however many drawables share it, and a face's targets are one base array
    /// however many heads carry them — so they are resolved, stamped and swept exactly where the
    /// meshes on them are.
    class MeshResolver
    {
    public:
        /// @param epoch the mirror's sweep stamp, read at every call. Borrowed, so that the mirror
        ///        and everything resolving into it cannot come to hold two answers.
        MeshResolver(SceneDesc& scene, const std::uint64_t& epoch)
            : mScene(scene)
            , mEpoch(epoch)
        {
        }

        /// What of a drawable there is to mirror: the geometry its triangles and attributes are
        /// read from, and what poses it, where something does.
        ///
        /// A skinned body's geometry is its **source** — the bind pose, which is what a pose is
        /// computed from on the device — and a morphed face's is its source too, with the base
        /// target standing in for its positions. Neither is the double-buffered copy a cull writes,
        /// which nothing here runs any more.
        struct Read
        {
            const osg::Geometry* mGeometry = nullptr;
            Deform mDeform = Deform::None;
            const SceneUtil::RigGeometry* mRig = nullptr;
            const SceneUtil::MorphGeometry* mMorph = nullptr;
        };

        /// Reads what a drawable is, in one virtual call for nearly everything in a cell.
        static Read readDrawable(const osg::Drawable& drawable);

        /// The mesh index for one drawable, adding it or posing it as its kind requires.
        ///
        /// @param material what the drawable wears, resolved first, which a mesh records as it
        ///        arrives — `MeshRange::mMaterial`.
        Index resolve(const osg::Drawable& drawable, const Read& read, Index material, ExtractionStats& stats);

        /// Drops every mesh this epoch did not meet, and collects the survivors into `live`.
        ///
        /// @return how many were dropped.
        std::uint32_t retire(std::vector<Index>& live);

    private:
        /// The scene's rig for a skin, added the first time the skin is met. Shared by every copy of
        /// the drawable, because the skin is.
        Index resolveRig(const SceneUtil::RigGeometry& rig);

        /// The same for a morph's targets, keyed on the base target every copy shares.
        Index resolveMorph(const SceneUtil::MorphGeometry& morph);

        /// Hands the scene this frame's bone rows for `mesh`: `RigGeometry::cull`'s own composition
        /// of each bone's inverse bind, its skeleton-space matrix and the skin transform, from the
        /// matrices the update traversal left.
        void poseRig(Index mesh, const SceneUtil::RigGeometry& rig);

        /// The same with the morph's weights, which its controller wrote under the update traversal.
        void poseMorph(Index mesh, const SceneUtil::MorphGeometry& morph);

        SceneDesc& mScene;
        const std::uint64_t& mEpoch;

        // Keyed on pointer identity, which OpenMW's resource cache and its optimizer's
        // SHARE_DUPLICATE_STATE pass together make meaningful: the same model loaded twice is the
        // same object, and equivalent state sets are collapsed into one.
        //
        // **Owning, which is what makes that identity sound.** What these hold outlives the graph
        // by one sweep, and a sweep is what lets go.
        Identity<const osg::Drawable> mMeshes;

        /// What the scene knows each skin and each set of morph targets as. Swept with the meshes: a
        /// rig no mesh named this epoch is a rig the scene has let go of.
        Identity<const SceneUtil::RigGeometry::InfluenceData> mRigs;
        Identity<const osg::Vec3Array> mMorphs;

        /// Which cards the content drew as two coincident sheets, so a ray offset can tell them
        /// from a wall.
        ShapeFold mShapeFold;

        // Refilled per drawable rather than reallocated, because a cell is tens of thousands of
        // them.
        std::vector<std::uint32_t> mIndexScratch;

        /// Where an overall normal is spread across a drawable's vertices. See `readVertices`.
        std::vector<osg::Vec3f> mFlatNormalScratch;

        // Refilled per rig and per morph, which a crowd is hundreds of.
        std::vector<std::uint32_t> mRunScratch;
        std::vector<Shaders::GpuInfluence> mInfluenceScratch;
        std::vector<osg::Vec3f> mOffsetScratch;
        std::vector<Shaders::GpuBone> mBoneScratch;
        std::vector<float> mWeightScratch;
    };
}
