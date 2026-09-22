#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <osg/Referenced>
#include <osg/Vec3f>

#include "deformertable.hpp"
#include "extractionstats.hpp"
#include "meshreader.hpp"
#include "mirroridentity.hpp"
#include "mirrorpass.hpp"
#include "runs.hpp"
#include "scenedesc.hpp"
#include "shaders/skinning.h"

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
    /// again is the same mesh posed again. The deformers are here because a mesh is what names
    /// one, and a skin is one `InfluenceData` however many drawables share it, as a morph's
    /// targets are one base array however many faces wear them.
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
        Index resolve(const osg::Drawable& drawable, const DrawableRead& read);

        /// The mesh index for a drawable somebody else has already read, adding it where the mirror
        /// does not hold it and stamping it where it does — the insertion half of `resolve`, for a
        /// reading made off the frame, under the drawable so a clone met by the walk resolves to
        /// this mesh. Standing only. One hold is taken on the entry until `release` gives it back.
        Index adopt(const osg::Drawable& drawable, const MeshReading& reading);

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

        /// Drops the deformers no mesh named this epoch. Nearly always two comparisons and nothing
        /// else, because a deformer goes stale only where a mesh on it died.
        void retireDeformers();

        /// Reserves the identity maps once, so no frame rehashes them. `SceneExtractor` states the
        /// budgets.
        void reserve(std::size_t meshes, std::size_t deformers)
        {
            mMeshes.reserve(meshes);
            mDeformers.reserve(deformers);
        }

    private:
        using DeformerEntry = Identity<const osg::Referenced>::Entry;

        /// What poses one drawable, as the mirror already holds it — the entry and not only the
        /// index, so the stamp does not `find` again for every posed part of a crowded cell.
        struct Held
        {
            Index mIndex = sNoIndex;
            DeformerEntry mEntry;
        };

        /// What a drawable's deformer is keyed on: the skin every copy of a rig shares, or the
        /// base target every copy of a morph shares. Both are `osg::Referenced`, so one map holds
        /// both and holds them alive — `ByAddress`.
        static const osg::Referenced* deformerKeyOf(const DrawableRead& read);

        /// Whether the deformer in `slot` is what `read` asks for: the same kind, and for a morph
        /// the same count of targets, because a set of targets grown or shrunk under the same
        /// base is a new set.
        bool fits(Index slot, const DrawableRead& read) const;

        /// The deformer this drawable stands on, where the mirror holds one, and `sNoIndex` where
        /// it does not. Stamps nothing, because whether the slot still fits is decided after this.
        Held holdDeformer(const DrawableRead& read);

        /// Adds the mesh a drawable the mirror is meeting afresh was read as: standing, on a
        /// deformer the mirror holds that fits it, or with a deformer of its own — both rows in one
        /// call of the scene's, and the identity entry added or moved only once they exist. An
        /// entry reached before the rows named nothing while the read could still throw. Throws
        /// where the drawable's skin or targets do not pose exactly its vertices, because a
        /// vertex count comes out of a content file.
        Index addMesh(const DrawableRead& read, const MeshReading& reading);

        /// Says the walk met what `holdDeformer` found, for a slot the fit test has kept, so a
        /// deformer is kept for as long as a mesh stands on it.
        void stampDeformer(const Held& held);

        /// Reads a skin into the scratch as the scene takes it: the groups flattened into a run
        /// per vertex. Throws where the skin names a vertex the mesh has not got. The spec spans
        /// the scratch, good until the next read.
        RigSpec readRig(const SceneUtil::RigGeometry& rig);

        /// The same for a morph's targets, laid end to end.
        MorphSpec readMorph(const SceneUtil::MorphGeometry& morph);

        /// Poses `mesh` where the drawable deforms, and counts it. Nothing where it stands. A rig's
        /// rows are `RigGeometry::cull`'s own composition of each bone's inverse bind, its
        /// skeleton-space matrix and the skin transform, from the matrices the update traversal
        /// left; a morph's weights are what its controller wrote under the update traversal.
        void pose(Index mesh, const DrawableRead& read, ExtractionStats& stats);

        SceneDesc& mScene;
        const MirrorPass& mPass;

        // Keyed on pointer identity, which OpenMW's resource cache and SHARE_DUPLICATE_STATE make
        // meaningful, and owning, which makes it sound: what these hold outlives the graph by one
        // sweep.
        Identity<const osg::Drawable> mMeshes{ mPass };

        /// What the scene knows each skin and each set of morph targets as. Swept with the meshes:
        /// a deformer no mesh named this epoch is one the scene has let go of.
        Identity<const osg::Referenced> mDeformers{ mPass };

        MeshReader mReader;

        // Refilled per rig and per morph, which a crowd is hundreds of.
        std::vector<std::uint32_t> mRunScratch;
        std::vector<Shaders::GpuInfluence> mInfluenceScratch;
        std::vector<osg::Vec3f> mOffsetScratch;
        std::vector<Shaders::GpuBone> mBoneScratch;
        std::vector<float> mWeightScratch;
        std::vector<PoseWord> mPoseScratch;
    };
}
