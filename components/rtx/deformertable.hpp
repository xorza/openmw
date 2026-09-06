#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>

#include "index.hpp"
#include "meshrange.hpp"
#include "shaders/skinning.h"
#include "spanallocator.hpp"

namespace Rtx
{
    /// What skins one bind pose: a run word per vertex and the influences the runs name, laid in the
    /// scene's shared tables. `Shaders::GpuInfluence` says what a run is.
    ///
    /// **Shared by every mesh built from one skin**, because that is what the content shares:
    /// `SceneUtil::RigGeometry` copies keep one `InfluenceData` between them, and a body part worn by
    /// a hundred people is one rig here and a hundred meshes. A rig outlives its last mesh by one
    /// sweep and goes with it.
    struct Rig
    {
        Index mRunOffset = 0;
        Index mInfluenceOffset = 0;
        Index mInfluenceCount = 0;

        /// Rows one pose of this rig takes, which is what every mesh on it is given.
        Index mBoneCount = 0;

        /// Vertices this rig skins, which every mesh on it must have exactly.
        Index mVertexCount = 0;

        /// How many meshes stand on it. Nought is a free slot.
        Index mUses = 0;
    };

    /// What morphs one base: every target's offsets laid end to end, target by target, in the
    /// scene's shared table. A pose is one weight per target.
    struct Morph
    {
        Index mOffsetsAt = 0;
        Index mTargetCount = 0;
        Index mVertexCount = 0;

        /// How many meshes stand on it. Nought is a free slot.
        Index mUses = 0;
    };

    /// What poses the meshes that deform: the rigs, the morphs, and the pose each mesh on one holds.
    ///
    /// **One type, because a deformer and the poses standing on it are one invariant.** A rig is
    /// shared by every mesh built from one skin and goes with the last of them, so the count, the
    /// runs behind it and the rows each mesh was given have to be released in one order, across six
    /// allocators and two free lists.
    ///
    /// **The mesh's own fields stay on the mesh.** Which deformer poses it, where its rows sit and
    /// whether it has been posed are facts about the mesh, so every call here takes the
    /// `MeshRange` rather than four indices copied out of it.
    class DeformerTable
    {
    public:
        /// Copies a skin's runs and influences into the shared tables and returns the rig's index.
        Index addRig(
            std::span<const std::uint32_t> runs, std::span<const Shaders::GpuInfluence> influences, Index boneCount);

        /// Copies a morph's offsets into the shared table and returns the morph's index.
        Index addMorph(std::span<const osg::Vec3f> offsets, Index targets);

        /// Gives `range` the runs its kind needs, and counts one more mesh on the deformer it names.
        ///
        /// Nothing for a mesh that stands. The rows it hands out are zeroed, which is a pose nothing
        /// can equal — `MeshRange::mPosed` says why the first pose counts regardless.
        void stand(MeshRange& range);

        /// Writes one mesh's bone rows. @return whether they differ from the ones it held.
        bool poseRig(const MeshRange& range, std::span<const Shaders::GpuBone> bones);

        /// The same for a morphed mesh's weights.
        bool poseMorph(const MeshRange& range, std::span<const float> weights);

        /// Gives a deforming mesh's runs back: its bind run, its pose run, and its rig's or morph's
        /// where this was the last mesh standing on it.
        void release(MeshRange& range);

        std::span<const Rig> getRigs() const { return mRigs; }
        std::span<const Morph> getMorphs() const { return mMorphs; }
        std::span<const std::uint32_t> getRuns() const { return mRuns; }
        std::span<const Shaders::GpuInfluence> getInfluences() const { return mInfluences; }
        std::span<const osg::Vec3f> getMorphOffsets() const { return mMorphOffsets; }
        std::span<const Shaders::GpuBone> getBones() const { return mBones; }
        std::span<const float> getWeights() const { return mWeights; }

        std::span<const Index> getArrivedRigs() const { return mArrivedRigs; }
        std::span<const Index> getArrivedMorphs() const { return mArrivedMorphs; }

        /// How many vertices the deforming meshes' bind poses take between them.
        Index getBindVertexCount() const { return mBindRuns.getEnd(); }

        std::span<const Shaders::GpuBone> getMeshBones(const MeshRange& range) const;
        std::span<const float> getMeshWeights(const MeshRange& range) const;

        void clearArrivals();

    private:
        std::vector<Rig> mRigs;
        std::vector<std::uint32_t> mRuns;
        std::vector<Shaders::GpuInfluence> mInfluences;
        std::vector<Morph> mMorphs;
        std::vector<osg::Vec3f> mMorphOffsets;
        std::vector<Shaders::GpuBone> mBones;
        std::vector<float> mWeights;

        std::vector<Index> mFreeRigs;
        std::vector<Index> mFreeMorphs;
        std::vector<Index> mArrivedRigs;
        std::vector<Index> mArrivedMorphs;

        /// The deforming meshes' bind poses and their bone rows and weights, and the rigs' and the
        /// morphs' own runs. Unblocked: a backend reaches each run by an address it is handed per
        /// dispatch, so nothing here has to keep an address across a growth.
        SpanAllocator mBindRuns;
        SpanAllocator mBoneRuns;
        SpanAllocator mWeightRuns;
        SpanAllocator mRigRuns;
        SpanAllocator mInfluenceRuns;
        SpanAllocator mMorphRuns;
    };
}
