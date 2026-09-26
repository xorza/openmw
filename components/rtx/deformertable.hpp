#pragma once

#include <cstddef>
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
    /// Sixteen bytes, which is what both poses are laid in: a bone is three words and a morph's
    /// weights fill words four at a time, zero-padded past the last. `Shaders::BONE_ALIGN` is why
    /// a word is this wide — a run of bones handed to the kernel claims that alignment of every
    /// address it is built from — and the device reads the same bytes it read when the two poses
    /// had a buffer each: three words are one `GpuBone`, and four floats in a row are four floats.
    struct alignas(16) PoseWord
    {
        float mValues[4];

        bool operator==(const PoseWord& other) const = default;
    };

    static_assert(sizeof(PoseWord) == Shaders::BONE_ALIGN, "a pose word is what a bone's reference is aligned to");
    static_assert(sizeof(Shaders::GpuBone) == 3 * sizeof(PoseWord), "a bone is three pose words");

    /// How many words `rows` of `kind` take: three a bone, a quarter a weight rounded up.
    constexpr Index poseWordsFor(const Deform kind, const Index rows)
    {
        return kind == Deform::Rig ? rows * 3 : (rows + 3) / 4;
    }

    /// Lays `bones` end to end as words, into `into`, refilled.
    void packBones(std::span<const Shaders::GpuBone> bones, std::vector<PoseWord>& into);

    /// Lays `weights` four to a word, the last word zero past the end, into `into`, refilled.
    void packWeights(std::span<const float> weights, std::vector<PoseWord>& into);

    /// Bone `at` of a pose laid by `packBones`.
    Shaders::GpuBone boneAt(std::span<const PoseWord> pose, Index at);

    /// Weight `at` of a pose laid by `packWeights`.
    float weightAt(std::span<const PoseWord> pose, Index at);

    /// What poses one kind of mesh: a skin, which names a run of words and a run of influences,
    /// or a set of morph targets, which names a run of offsets. Whichever it is not is empty, and
    /// releasing an empty run is nothing. Shared by every mesh built from one skin, because
    /// `SceneUtil::RigGeometry` copies keep one `InfluenceData` between them, and by every copy
    /// of a face; a deformer goes with its last mesh.
    struct Deformer
    {
        Deform mKind = Deform::None;

        /// One run word per vertex this rig skins. Never empty for a rig, because an allocator
        /// hands out no run of nothing, which is why the run is kept rather than the count.
        Run mRuns;

        /// The influences those runs name. Never empty for a rig, for the same reason.
        Run mInfluences;

        /// Every target's offsets, target by target and `getVertexCount` apiece, for a morph.
        Run mOffsets;

        /// Rows one pose takes: the rig's bones, or the morph's targets, the base's included.
        Index mRows = 0;

        /// Vertices this deformer moves, which every mesh on it must have exactly.
        Index getVertexCount() const
        {
            if (mKind == Deform::Rig)
                return mRuns.mCount;

            return mRows > 0 ? mOffsets.mCount / mRows : 0;
        }

        /// Words one pose of this deformer takes, which is what every mesh on it is given.
        Index getPoseWords() const { return poseWordsFor(mKind, mRows); }
    };

    /// A skin as a mesh arrives with it: the run word per vertex, the influences those words
    /// name, and how many bones a pose has. Spans into the reader's own buffers, good for the
    /// call that adds it.
    struct RigSpec
    {
        std::span<const std::uint32_t> mRuns;
        std::span<const Shaders::GpuInfluence> mInfluences;
        Index mBones = 0;

        Index getVertexCount() const { return static_cast<Index>(mRuns.size()); }
    };

    /// A set of morph targets as a mesh arrives with them: every target's offsets end to end, the
    /// base's included, and how many targets there are.
    struct MorphSpec
    {
        std::span<const osg::Vec3f> mOffsets;
        Index mTargets = 0;

        Index getVertexCount() const { return mTargets > 0 ? static_cast<Index>(mOffsets.size() / mTargets) : 0; }
    };

    /// What poses the meshes that deform: the deformers, and the pose each mesh on one holds.
    /// One type, because a deformer's rows, the runs behind it and the words each mesh was given
    /// have to be released in one order across five allocators. A mesh standing on a deformer is
    /// one hold on its row. Every call takes the `MeshRange`, because which deformer poses a mesh
    /// is the mesh's own fact. One row table, one arrival set and one pose buffer for both kinds,
    /// because everything but the kernel that reads them is the same bookkeeping.
    class DeformerTable
    {
    public:
        /// Copies a skin's runs and influences into the shared tables and returns the rig's index.
        /// The row arrives with nothing on it; `SceneDesc::addMesh` is what calls this, and stands
        /// the first mesh on the row in the same call, because a row no mesh stands on is one
        /// nothing frees.
        Index addRig(const RigSpec& rig);

        /// The same for a morph's targets.
        Index addMorph(const MorphSpec& morph);

        /// Gives `range` the runs its kind needs, and counts one more mesh on the deformer it names.
        /// Nothing for a mesh that stands. The words it hands out are zeroed, which is a pose
        /// nothing can equal — `MeshRange::mPosed` says why the first pose counts regardless.
        void stand(MeshRange& range);

        /// Writes one mesh's pose, laid as `packBones` or `packWeights` lays it, over the words it
        /// holds. @return whether they differ from the ones it held.
        bool pose(const MeshRange& range, std::span<const PoseWord> words);

        /// Gives a deforming mesh's runs back: its bind run, its pose run, and its deformer's
        /// where this was the last mesh standing on it.
        void release(MeshRange& range);

        std::span<const Deformer> getDeformers() const { return mDeformers.getRows(); }
        std::span<const std::uint32_t> getRuns() const { return mRuns.getAll(); }
        std::span<const Shaders::GpuInfluence> getInfluences() const { return mInfluences.getAll(); }
        std::span<const osg::Vec3f> getMorphOffsets() const { return mOffsets.getAll(); }
        std::span<const PoseWord> getPoses() const { return mPoses.getAll(); }

        /// Which deformer slots have been written since the last `clearArrivals`, each named once.
        std::span<const Index> getArrived() const { return mArrived.getSlots(); }

        /// How many vertices the deforming meshes' bind poses take between them.
        Index getBindVertexCount() const { return mBindRuns.getEnd(); }

        /// The pose a mesh holds, in words.
        std::span<const PoseWord> getMeshPose(const MeshRange& range) const;

        /// Settles what `release` took out of the arrivals, so they can be read again. Called where
        /// a sweep ends, which is the only thing that releases a deformer.
        void compact();

        void clearArrivals();

        /// Whether `deformer` holds a rig or a set of targets: what `SceneDesc::isConsistent`
        /// asks beside the holds, because a live row with none is one nothing frees.
        bool isLive(Index deformer) const { return mDeformers.isLive(deformer); }
        std::size_t getLiveCount() const { return mDeformers.getLiveCount(); }

        /// How many meshes stand on a deformer. Nought is a free slot, or one the mesh that
        /// brought it has not stood on yet — a window `SceneDesc::addMesh` closes in the same call.
        std::uint32_t getHolds(Index deformer) const { return mDeformers.getHolds(deformer); }

    private:
        /// Files `deformer` in a slot and among the arrivals.
        Index take(const Deformer& deformer);

        SlotRows<Deformer> mDeformers;

        /// Unblocked, unlike the bind runs below: a backend reaches each of these by an address
        /// it is handed per dispatch, so nothing here has to keep an address across a growth.
        RunBuffer<std::uint32_t> mRuns;
        RunBuffer<Shaders::GpuInfluence> mInfluences;
        RunBuffer<osg::Vec3f> mOffsets;

        /// Every mesh's pose, in words, whichever kind poses it.
        RunBuffer<PoseWord> mPoses;

        /// Which deformer slots have been written since the last `clearArrivals`. A set, because
        /// a deformer that arrives and is released inside one sweep must leave; `SlotSet` and not
        /// `SlotChanges`, because a deformer's storage is a run in a shared buffer and nothing has
        /// to be told one went.
        SlotSet mArrived;

        /// Where each deforming mesh's vertices sit among the deforming meshes alone, which is what
        /// both a bind table and a pose table are indexed by. Blocked like the scene's own vertices,
        /// because the address handed to a refit covers one allocation.
        RunAllocator mBindRuns{ Shaders::VERTEX_BLOCK };
    };
}
