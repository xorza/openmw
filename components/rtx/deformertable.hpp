#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>

#include "index.hpp"
#include "meshrange.hpp"
#include "runallocator.hpp"
#include "runbuffer.hpp"
#include "shaders/scene.h"
#include "shaders/skinning.h"
#include "slotrows.hpp"
#include "slotset.hpp"

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
        /// One run word per vertex this rig skins.
        Run mRuns;

        /// The influences those runs name.
        ///
        /// **Never empty, and that is why the run is kept rather than the count.** An allocator
        /// hands out no run of nothing, so a mesh whose every vertex follows no bone still holds one
        /// influence it never reads — and a stored count of nought would have the upload and the
        /// release disagree with what was taken.
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

    /// What poses the meshes that deform: the rigs, the morphs, and the pose each mesh on one holds.
    ///
    /// **One type, because a deformer and the poses standing on it are one invariant.** A rig is
    /// shared by every mesh built from one skin and goes with the last of them, so the count, the
    /// runs behind it and the rows each mesh was given have to be released in one order, across six
    /// allocators and two free lists. The count is the one `SlotRows` keeps: a mesh standing on a
    /// rig is one hold on its row, and the last hold given back is what frees it.
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

        /// Settles what `release` took out of the arrivals, so they can be read again.
        ///
        /// **Called where a sweep ends and nowhere else**, because a sweep is the only thing that
        /// releases a deformer. `SlotSet::remove` leaves its list holding the slot until a pass
        /// settles it, which is what makes a sweep of thousands one pass rather than thousands.
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

        /// Which rig and morph slots have been written since the last `clearArrivals`.
        ///
        /// **Sets and not lists**, because a deformer that arrives and is released inside one sweep
        /// must leave. Taking it out of a list is a scan and a shift of the whole list per released
        /// rig, on the frame a cell leaves; a set marks a byte and settles the lot in one pass.
        ///
        /// **`SlotSet` and not `SlotChanges`**, because there is no freed list to keep: a rig's
        /// storage is a run in a shared buffer, so `SkinTables::writeRigs` never has to be told one
        /// went — the next rig to land in the run is what writes it again.
        SlotSet mArrivedRigs;
        SlotSet mArrivedMorphs;

        /// Where each deforming mesh's vertices sit among the deforming meshes alone, which is what
        /// both a bind table and a pose table are indexed by.
        ///
        /// **Blocked like the scene's own vertices**, because a backend holds the poses in a table
        /// blocked the same way: a pose that straddled a block would be a run split across two
        /// allocations, and the address handed to a refit covers one. The tail of a block too short
        /// for the next body is a hole like any other, and a block is a quarter of a million
        /// vertices against a body's couple of thousand.
        RunAllocator mBindRuns{ Shaders::VERTEX_BLOCK };
    };
}
