#include "deformertable.hpp"

#include <algorithm>
#include <cassert>

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

        // A rig with no influence at all still takes a run of one, because a backend addresses the
        // run whether or not it is read. Zeroed rather than left as the last tenant wrote it, so
        // the scene digest, which hashes whole, agrees between two runs.
        const Run words = mRuns.allocate(runs);
        const Run shares = influences.empty() ? mInfluences.allocateZeroed(1) : mInfluences.allocate(influences);

        const Index index = mRigs.take(Rig{
            .mRuns = words,
            .mInfluences = shares,
            .mBoneCount = boneCount,
        });

        mArrivedRigs.addMakingRoom(index);
        return index;
    }

    Index DeformerTable::addMorph(std::span<const osg::Vec3f> offsets, Index targets)
    {
        assert(targets > 0 && offsets.size() % targets == 0 && !offsets.empty());

        const Run run = mMorphOffsets.allocate(offsets);

        const Index index = mMorphs.take(Morph{
            .mOffsets = run,
            .mTargetCount = targets,
        });

        mArrivedMorphs.addMakingRoom(index);
        return index;
    }

    namespace
    {
        /// Writes `pose` over `held` where the two differ, and says whether they did. Compared
        /// rather than trusted, because the walk poses every rig it meets and cannot know which of
        /// them the engine animated.
        template <class T>
        bool takePose(std::span<const T> pose, std::span<T> held, bool posed)
        {
            assert(pose.size() == held.size() && "a pose written over a run of another length");

            if (posed && std::equal(pose.begin(), pose.end(), held.begin()))
                return false;

            std::copy(pose.begin(), pose.end(), held.begin());
            return true;
        }
    }

    bool DeformerTable::poseRig(const MeshRange& range, std::span<const Shaders::GpuBone> bones)
    {
        assert(range.mDeform == Deform::Rig && "a pose of rows for a mesh no rig skins");

        const Index rows = mRigs.at(range.mDeformer).mBoneCount;
        assert(bones.size() == rows && "one row per bone of the rig, and no other count");

        return takePose(bones, mBones.in(Run{ .mOffset = range.mPoseOffset, .mCount = rows }), range.mPosed);
    }

    bool DeformerTable::poseMorph(const MeshRange& range, std::span<const float> weights)
    {
        assert(range.mDeform == Deform::Morph && "a pose of weights for a mesh no morph moves");

        const Index targets = mMorphs.at(range.mDeformer).mTargetCount;
        assert(weights.size() == targets && "one weight per target of the morph, and no other count");

        return takePose(weights, mWeights.in(Run{ .mOffset = range.mPoseOffset, .mCount = targets }), range.mPosed);
    }

    std::span<const Shaders::GpuBone> DeformerTable::getMeshBones(const MeshRange& range) const
    {
        assert(range.mDeform == Deform::Rig);
        return getBones().subspan(range.mPoseOffset, mRigs.at(range.mDeformer).mBoneCount);
    }

    std::span<const float> DeformerTable::getMeshWeights(const MeshRange& range) const
    {
        assert(range.mDeform == Deform::Morph);
        return getWeights().subspan(range.mPoseOffset, mMorphs.at(range.mDeformer).mTargetCount);
    }

    void DeformerTable::release(MeshRange& range)
    {
        if (range.mDeform == Deform::None)
            return;

        mBindRuns.release(Run{ .mOffset = range.mBindOffset, .mCount = range.mVertices.mCount });

        // **The rig or the morph goes with its last mesh**, and its runs with it. Nothing downstream
        // is told: what a backend holds of a rig is data at an offset, read by no frame once no mesh
        // names it, and the next rig to land in the run is what names it again.
        if (range.mDeform == Deform::Rig)
        {
            Rig& rig = mRigs.at(range.mDeformer);
            mBones.release(Run{ .mOffset = range.mPoseOffset, .mCount = rig.mBoneCount });

            if (mRigs.drop(range.mDeformer))
            {
                mRuns.release(rig.mRuns);
                mInfluences.release(rig.mInfluences);
                rig = Rig{};
                mRigs.free(range.mDeformer);
                mArrivedRigs.remove(range.mDeformer);
            }
        }
        else
        {
            Morph& morph = mMorphs.at(range.mDeformer);
            mWeights.release(Run{ .mOffset = range.mPoseOffset, .mCount = morph.mTargetCount });

            if (mMorphs.drop(range.mDeformer))
            {
                mMorphOffsets.release(morph.mOffsets);
                morph = Morph{};
                mMorphs.free(range.mDeformer);
                mArrivedMorphs.remove(range.mDeformer);
            }
        }

        range.mDeform = Deform::None;
        range.mDeformer = sNoIndex;
        range.mPosed = false;
    }

    void DeformerTable::stand(MeshRange& range)
    {
        if (range.mDeform == Deform::None)
            return;

        // **A run in the bind table whichever kind it is**, because the bind pose is the mesh's
        // vertices and both kinds are computed from them.
        range.mBindOffset = mBindRuns.allocate(range.mVertices.mCount).mOffset;

        // **And a run of rows or of weights, zeroed.** Zero is a pose nothing can equal, and
        // `MeshRange::mPosed` is what says the first pose names the mesh regardless.
        if (range.mDeform == Deform::Rig)
        {
            mRigs.hold(range.mDeformer);
            range.mPoseOffset = mBones.allocateZeroed(mRigs.at(range.mDeformer).mBoneCount).mOffset;
        }
        else
        {
            mMorphs.hold(range.mDeformer);
            range.mPoseOffset = mWeights.allocateZeroed(mMorphs.at(range.mDeformer).mTargetCount).mOffset;
        }
    }

    void DeformerTable::compact()
    {
        mArrivedRigs.compact();
        mArrivedMorphs.compact();
    }

    void DeformerTable::clearArrivals()
    {
        mArrivedRigs.clear();
        mArrivedMorphs.clear();
    }
}
