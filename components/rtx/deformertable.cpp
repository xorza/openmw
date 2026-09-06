#include "deformertable.hpp"

#include <algorithm>
#include <cassert>

#include "slotrows.hpp"

namespace Rtx
{
    Index DeformerTable::addRig(
        std::span<const std::uint32_t> runs, std::span<const Shaders::GpuInfluence> influences, Index boneCount)
    {
        assert(!runs.empty());
        assert(boneCount > 0);
        assert(std::all_of(runs.begin(), runs.end(), [&](std::uint32_t run) {
            const std::uint32_t first = run >> Shaders::RUN_COUNT_BITS;
            const std::uint32_t count = run & Shaders::RUN_COUNT_MASK;
            return first + count <= influences.size();
        }) && "a run past the influences it was handed");
        assert(std::all_of(influences.begin(), influences.end(), [&](const Shaders::GpuInfluence& influence) {
            return influence.mBone < boneCount;
        }) && "an influence naming a bone the rig has not got");

        // **A rig with no influence at all still takes a run of one**, because an allocator hands
        // out no run of nothing and a backend addresses the run whether or not it is read: a mesh
        // whose every vertex follows no bone is the zero matrix everywhere, as the rasterizer has it.
        const Span words = mRigRuns.allocate(static_cast<Index>(runs.size()));
        const Span shares = mInfluenceRuns.allocate(std::max<Index>(1, static_cast<Index>(influences.size())));

        if (mRuns.size() < mRigRuns.getEnd())
            mRuns.resize(mRigRuns.getEnd());
        if (mInfluences.size() < mInfluenceRuns.getEnd())
            mInfluences.resize(mInfluenceRuns.getEnd());

        std::copy(runs.begin(), runs.end(), mRuns.begin() + words.mOffset);
        std::copy(influences.begin(), influences.end(), mInfluences.begin() + shares.mOffset);

        const Index index = takeSlot(mRigs, mFreeRigs,
            Rig{
                .mRunOffset = words.mOffset,
                .mInfluenceOffset = shares.mOffset,
                .mInfluenceCount = static_cast<Index>(influences.size()),
                .mBoneCount = boneCount,
                .mVertexCount = static_cast<Index>(runs.size()),
            });

        mArrivedRigs.push_back(index);
        return index;
    }

    Index DeformerTable::addMorph(std::span<const osg::Vec3f> offsets, Index targets)
    {
        assert(targets > 0 && offsets.size() % targets == 0 && !offsets.empty());

        const Span run = mMorphRuns.allocate(static_cast<Index>(offsets.size()));
        if (mMorphOffsets.size() < mMorphRuns.getEnd())
            mMorphOffsets.resize(mMorphRuns.getEnd());

        std::copy(offsets.begin(), offsets.end(), mMorphOffsets.begin() + run.mOffset);

        const Index index = takeSlot(mMorphs, mFreeMorphs,
            Morph{
                .mOffsetsAt = run.mOffset,
                .mTargetCount = targets,
                .mVertexCount = static_cast<Index>(offsets.size() / targets),
            });

        mArrivedMorphs.push_back(index);
        return index;
    }

    namespace
    {
        /// Writes `pose` over `held` where the two differ, and says whether they did.
        ///
        /// **Compared rather than trusted**, because the walk poses every rig it meets and cannot
        /// know which of them the engine animated. A first pose always counts: what the slot held
        /// before it is nothing a pose can equal.
        template <class T>
        bool takePose(std::span<const T> pose, T* held, bool posed)
        {
            if (posed && std::equal(pose.begin(), pose.end(), held))
                return false;

            std::copy(pose.begin(), pose.end(), held);
            return true;
        }
    }

    bool DeformerTable::poseRig(const MeshRange& range, std::span<const Shaders::GpuBone> bones)
    {
        assert(range.mDeform == Deform::Rig && "a pose of rows for a mesh no rig skins");
        assert(bones.size() == mRigs[range.mDeformer].mBoneCount && "one row per bone of the rig, and no other count");

        return takePose(bones, mBones.data() + range.mPoseOffset, range.mPosed);
    }

    bool DeformerTable::poseMorph(const MeshRange& range, std::span<const float> weights)
    {
        assert(range.mDeform == Deform::Morph && "a pose of weights for a mesh no morph moves");
        assert(weights.size() == mMorphs[range.mDeformer].mTargetCount
            && "one weight per target of the morph, and no other count");

        return takePose(weights, mWeights.data() + range.mPoseOffset, range.mPosed);
    }

    std::span<const Shaders::GpuBone> DeformerTable::getMeshBones(const MeshRange& range) const
    {
        assert(range.mDeform == Deform::Rig);
        return std::span(mBones).subspan(range.mPoseOffset, mRigs[range.mDeformer].mBoneCount);
    }

    std::span<const float> DeformerTable::getMeshWeights(const MeshRange& range) const
    {
        assert(range.mDeform == Deform::Morph);
        return std::span(mWeights).subspan(range.mPoseOffset, mMorphs[range.mDeformer].mTargetCount);
    }

    void DeformerTable::release(MeshRange& range)
    {
        if (range.mDeform == Deform::None)
            return;

        mBindRuns.release(Span{ .mOffset = range.mBindOffset, .mCount = range.mVertexCount });

        // **The rig or the morph goes with its last mesh**, and its runs with it. Nothing downstream
        // is told: what a backend holds of a rig is data at an offset, read by no frame once no mesh
        // names it, and the next rig to land in the run is what names it again.
        if (range.mDeform == Deform::Rig)
        {
            Rig& rig = mRigs[range.mDeformer];
            mBoneRuns.release(Span{ .mOffset = range.mPoseOffset, .mCount = rig.mBoneCount });

            assert(rig.mUses > 0 && "a rig given back more often than it was stood on");
            if (--rig.mUses == 0)
            {
                mRigRuns.release(Span{ .mOffset = rig.mRunOffset, .mCount = rig.mVertexCount });
                mInfluenceRuns.release(
                    Span{ .mOffset = rig.mInfluenceOffset, .mCount = std::max<Index>(1, rig.mInfluenceCount) });
                rig = Rig{};
                mFreeRigs.push_back(range.mDeformer);
                std::erase(mArrivedRigs, range.mDeformer);
            }
        }
        else
        {
            Morph& morph = mMorphs[range.mDeformer];
            mWeightRuns.release(Span{ .mOffset = range.mPoseOffset, .mCount = morph.mTargetCount });

            assert(morph.mUses > 0 && "a morph given back more often than it was stood on");
            if (--morph.mUses == 0)
            {
                mMorphRuns.release(
                    Span{ .mOffset = morph.mOffsetsAt, .mCount = morph.mTargetCount * morph.mVertexCount });
                morph = Morph{};
                mFreeMorphs.push_back(range.mDeformer);
                std::erase(mArrivedMorphs, range.mDeformer);
            }
        }

        range.mDeform = Deform::None;
        range.mDeformer = sNoIndex;
        range.mPosed = false;
    }

    void DeformerTable::stand(MeshRange& range)
    {
        // **A run in the bind table and a run of rows or weights, for a mesh that deforms.** The
        // rows are zeroed, which is a pose nothing can equal, and `MeshRange::mPosed` is what says
        // the first pose names the mesh regardless. Grown to what the allocator reaches, never
        // shrunk, exactly as the vertex buffers are.
        if (range.mDeform == Deform::Rig)
        {
            Rig& rig = mRigs[range.mDeformer];
            ++rig.mUses;
            range.mBindOffset = mBindRuns.allocate(range.mVertexCount).mOffset;
            range.mPoseOffset = mBoneRuns.allocate(rig.mBoneCount).mOffset;
            if (mBones.size() < mBoneRuns.getEnd())
                mBones.resize(mBoneRuns.getEnd());

            std::fill_n(mBones.begin() + range.mPoseOffset, rig.mBoneCount, Shaders::GpuBone{});
        }
        else if (range.mDeform == Deform::Morph)
        {
            Morph& morph = mMorphs[range.mDeformer];
            ++morph.mUses;
            range.mBindOffset = mBindRuns.allocate(range.mVertexCount).mOffset;
            range.mPoseOffset = mWeightRuns.allocate(morph.mTargetCount).mOffset;
            if (mWeights.size() < mWeightRuns.getEnd())
                mWeights.resize(mWeightRuns.getEnd());

            std::fill_n(mWeights.begin() + range.mPoseOffset, morph.mTargetCount, 0.0f);
        }
    }

    void DeformerTable::clearArrivals()
    {
        mArrivedRigs.clear();
        mArrivedMorphs.clear();
    }
}
