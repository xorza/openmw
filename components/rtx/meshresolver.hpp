#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Vec2f>
#include <osg/Vec3f>

// For `RigGeometry::InfluenceData`, which is what a rig is keyed on: a nested type cannot be
// forward-declared. It brings `osg::Vec3Array`, which a morph is keyed on, with it.
#include <components/sceneutil/riggeometry.hpp>

#include "meshreader.hpp"
#include "mirroridentity.hpp"
#include "runs.hpp"
#include "scenedesc.hpp"
#include "shaders/skinning.h"
#include "walk.hpp"

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
    /// Turns the drawables a walk met into the scene's meshes, and poses the ones that deform.
    /// Keyed on the drawable: a crate met again is the crate already uploaded, and a body met
    /// again is the same mesh posed again. The rigs and the morphs are here because a mesh is what
    /// names one, and a skin is one `InfluenceData` however many drawables share it.
    class MeshResolver
    {
    public:
        /// @param pass the walk in progress: its sweep stamp and its counts, read at every call.
        ///        Borrowed, so that the mirror and everything resolving into it cannot come to hold
        ///        two answers.
        MeshResolver(SceneDesc& scene, const MirrorPass& pass)
            : mScene(scene)
            , mPass(pass)
        {
        }

        /// The mesh index for one drawable, adding it or posing it as its kind requires.
        ///
        /// @param material what the drawable wears, resolved first, which a mesh records as it
        ///        arrives — `MeshRange::mMaterial`.
        Index resolve(const osg::Drawable& drawable, const DrawableRead& read, Index material);

        /// The mesh index for a drawable somebody else has already read, adding it where the mirror
        /// does not hold it and stamping it where it does — the insertion half of `resolve`, for a
        /// reading made off the frame, under the drawable so a clone met by the walk resolves to
        /// this mesh. Standing only. One hold is taken on the entry until `release` gives it back.
        Index adopt(const osg::Drawable& drawable, const MeshReading& reading, Index material);

        /// Gives one `adopt` back. The mirror must hold `drawable`, which it does for as long as
        /// anything holds it.
        void release(const osg::Drawable& drawable);

        /// Whether every mesh the map holds was met this epoch — see `Kept::whole`. What the
        /// mirror asks before it sweeps, because the survivor list this fills is read beside the
        /// material resolver's.
        bool whole() const { return mMeshes.whole(); }

        /// Drops every mesh neither this epoch nor a hold keeps, and collects the survivors into
        /// `live`.
        void retire(std::vector<Index>& live);

        /// Drops the rigs and the morph targets no mesh named this epoch. Nearly always two
        /// comparisons and nothing else, because a deformer goes stale only where a mesh on it died.
        void retireDeformers();

        /// Reserves the identity maps once, so no frame rehashes them. `SceneExtractor` states the
        /// budgets.
        void reserve(std::size_t meshes, std::size_t deformers)
        {
            mMeshes.reserve(meshes);
            mRigs.reserve(deformers);
            mMorphs.reserve(deformers);
        }

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

        /// What poses one drawable, as the mirror already holds it — the entry and not only the
        /// index, so the stamp does not `find` again for every posed part of a crowded cell.
        struct Held
        {
            Index mIndex = sNoIndex;

            Identity<const SceneUtil::RigGeometry::InfluenceData>::Entry mRig;
            Identity<const osg::Vec3Array>::Entry mMorph;
        };

        /// The deformer this drawable stands on, where the mirror holds one, and `sNoIndex` where
        /// it does not. Stamps nothing, because whether the slot still fits is decided after this.
        Held holdDeformer(const DrawableRead& read);

        /// The same for a drawable the mirror is meeting afresh: the deformer added and stamped,
        /// or `sNoIndex` where the drawable stands. Throws where it does not pose exactly
        /// `vertices`, because a vertex count comes out of a content file.
        Index addDeformer(const DrawableRead& read, std::size_t vertices);

        /// Says the walk met what `holdDeformer` found, for a slot the fit test has kept, so a
        /// deformer is kept for as long as a mesh stands on it.
        void stampDeformer(const DrawableRead& read, const Held& held);

        /// Poses `mesh` where the drawable deforms, and counts it. Nothing where it stands.
        void pose(Index mesh, const DrawableRead& read, ExtractionStats& stats);

        SceneDesc& mScene;
        const MirrorPass& mPass;

        // Keyed on pointer identity, which OpenMW's resource cache and SHARE_DUPLICATE_STATE make
        // meaningful, and owning, which makes it sound: what these hold outlives the graph by one
        // sweep.
        Identity<const osg::Drawable> mMeshes{ mPass };

        /// What the scene knows each skin and each set of morph targets as. Swept with the meshes: a
        /// rig no mesh named this epoch is a rig the scene has let go of.
        Identity<const SceneUtil::RigGeometry::InfluenceData> mRigs{ mPass };
        Identity<const osg::Vec3Array> mMorphs{ mPass };

        MeshReader mReader;

        // Refilled per rig and per morph, which a crowd is hundreds of.
        std::vector<std::uint32_t> mRunScratch;
        std::vector<Shaders::GpuInfluence> mInfluenceScratch;
        std::vector<osg::Vec3f> mOffsetScratch;
        std::vector<Shaders::GpuBone> mBoneScratch;
        std::vector<float> mWeightScratch;
    };
}
