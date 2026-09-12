#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>

#include "mesh.hpp"
#include "runs.hpp"
#include "shaders/scene.h"
#include "shaders/skinning.h"
#include "slots.hpp"

namespace Rtx
{
    /// What skins one bind pose: a run word per vertex and the influences the runs name, laid in the
    /// scene's shared tables. Shared by every mesh built from one skin, because
    /// `SceneUtil::RigGeometry` copies keep one `InfluenceData` between them. A rig goes with its
    /// last mesh.
    struct Rig
    {
        /// One run word per vertex this rig skins.
        Run mRuns;

        /// The influences those runs name. Never empty, because an allocator hands out no run of
        /// nothing, which is why the run is kept rather than the count.
        Run mInfluences;

        /// Rows one pose of this rig takes, which is what every mesh on it is given.
        Index mBoneCount = 0;

        /// Vertices this rig skins, which every mesh on it must have exactly. One run word apiece.
        Index getVertexCount() const { return mRuns.mCount; }
    };

    /// What morphs one base: every target's offsets laid end to end, target by target, in the
    /// scene's shared table. A pose is one weight per target.
    struct Morph
    {
        /// Every target's offsets, target by target and `getVertexCount` apiece.
        Run mOffsets;

        Index mTargetCount = 0;

        /// Vertices this morph moves, which every mesh on it must have exactly.
        Index getVertexCount() const { return mTargetCount > 0 ? mOffsets.mCount / mTargetCount : 0; }
    };

    /// What poses the meshes that deform: the rigs, the morphs, and the pose each mesh on one
    /// holds. One type, because a rig's count, the runs behind it and the rows each mesh was given
    /// have to be released in one order across six allocators. A mesh standing on a rig is one
    /// hold on its row. Every call takes the `MeshRange`, because which deformer poses a mesh is
    /// the mesh's own fact.
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

        std::span<const Rig> getRigs() const { return mRigs.getRows(); }
        std::span<const Morph> getMorphs() const { return mMorphs.getRows(); }
        std::span<const std::uint32_t> getRuns() const { return mRuns.getAll(); }
        std::span<const Shaders::GpuInfluence> getInfluences() const { return mInfluences.getAll(); }
        std::span<const osg::Vec3f> getMorphOffsets() const { return mMorphOffsets.getAll(); }
        std::span<const Shaders::GpuBone> getBones() const { return mBones.getAll(); }
        std::span<const float> getWeights() const { return mWeights.getAll(); }

        std::span<const Index> getArrivedRigs() const { return mArrivedRigs.getSlots(); }
        std::span<const Index> getArrivedMorphs() const { return mArrivedMorphs.getSlots(); }

        /// How many meshes stand on a rig or a morph. Nought is a free slot.
        std::uint32_t getRigHolds(Index rig) const { return mRigs.getHolds(rig); }
        std::uint32_t getMorphHolds(Index morph) const { return mMorphs.getHolds(morph); }

        /// How many vertices the deforming meshes' bind poses take between them.
        Index getBindVertexCount() const { return mBindRuns.getEnd(); }

        std::span<const Shaders::GpuBone> getMeshBones(const MeshRange& range) const;
        std::span<const float> getMeshWeights(const MeshRange& range) const;

        /// Settles what `release` took out of the arrivals, so they can be read again. Called where
        /// a sweep ends, which is the only thing that releases a deformer.
        void compact();

        void clearArrivals();

    private:
        SlotRows<Rig> mRigs;
        SlotRows<Morph> mMorphs;

        /// **Unblocked**, unlike the bind runs below: a backend reaches each of these by an address
        /// it is handed per dispatch, so nothing here has to keep an address across a growth.
        RunBuffer<std::uint32_t> mRuns;
        RunBuffer<Shaders::GpuInfluence> mInfluences;
        RunBuffer<osg::Vec3f> mMorphOffsets;
        RunBuffer<Shaders::GpuBone> mBones;
        RunBuffer<float> mWeights;

        /// Which rig and morph slots have been written since the last `clearArrivals`. Sets, because
        /// a deformer that arrives and is released inside one sweep must leave; `SlotSet` and not
        /// `SlotChanges`, because a rig's storage is a run in a shared buffer and nothing has to be
        /// told one went.
        SlotSet mArrivedRigs;
        SlotSet mArrivedMorphs;

        /// Where each deforming mesh's vertices sit among the deforming meshes alone, which is what
        /// both a bind table and a pose table are indexed by. Blocked like the scene's own vertices,
        /// because the address handed to a refit covers one allocation.
        RunAllocator mBindRuns{ Shaders::VERTEX_BLOCK };
    };
}
