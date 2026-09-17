#include "deformertable.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>

#include <osg/Vec4f>

namespace Rtx
{
    void packBones(const std::span<const Shaders::GpuBone> bones, std::vector<PoseWord>& into)
    {
        // Word by word rather than one copy of the bytes: `osg::Vec4f` is not trivially copyable
        // as the compiler counts it, and a row is four floats either way.
        into.resize(bones.size() * 3);
        for (std::size_t at = 0; at < bones.size(); ++at)
            for (std::size_t row = 0; row < 3; ++row)
            {
                const osg::Vec4f& values = bones[at].mRows[row];
                into[at * 3 + row] = PoseWord{ { values.x(), values.y(), values.z(), values.w() } };
            }
    }

    void packWeights(const std::span<const float> weights, std::vector<PoseWord>& into)
    {
        // Zeroed whole and then written, so the padding past the last weight is a value two runs
        // agree on: the compare in `pose` reads the whole run, and the digest hashes it.
        into.assign(poseWordsFor(Deform::Morph, static_cast<Index>(weights.size())), PoseWord{});
        if (!weights.empty())
            std::memcpy(into.data(), weights.data(), weights.size_bytes());
    }

    Shaders::GpuBone boneAt(const std::span<const PoseWord> pose, const Index at)
    {
        assert(std::size_t{ at } * 3 + 3 <= pose.size() && "a bone past the pose");

        Shaders::GpuBone bone;
        for (std::size_t row = 0; row < 3; ++row)
        {
            const PoseWord& word = pose[std::size_t{ at } * 3 + row];
            bone.mRows[row] = osg::Vec4f(word.mValues[0], word.mValues[1], word.mValues[2], word.mValues[3]);
        }
        return bone;
    }

    float weightAt(const std::span<const PoseWord> pose, const Index at)
    {
        assert(std::size_t{ at } / 4 < pose.size() && "a weight past the pose");
        return pose[at / 4].mValues[at % 4];
    }

    Index DeformerTable::take(const Deformer& deformer)
    {
        const Index index = mDeformers.take(deformer);
        mArrived.addMakingRoom(index);
        return index;
    }

    Index DeformerTable::addRig(const RigSpec& rig)
    {
        const std::span<const std::uint32_t> runs = rig.mRuns;
        const std::span<const Shaders::GpuInfluence> influences = rig.mInfluences;

        assert(!runs.empty());
        assert(rig.mBones > 0);
        assert(std::all_of(runs.begin(), runs.end(), [&](std::uint32_t run) {
            const std::uint32_t first = run >> Shaders::RUN_COUNT_BITS;
            const std::uint32_t count = run & Shaders::RUN_COUNT_MASK;
            return first + count <= influences.size();
        }) && "a run past the influences it was handed");
        assert(std::all_of(influences.begin(), influences.end(), [&](const Shaders::GpuInfluence& influence) {
            return influence.mBone < rig.mBones;
        }) && "an influence naming a bone the rig has not got");

        // A rig with no influence at all still takes a run of one, because a backend addresses the
        // run whether or not it is read. Zeroed rather than left as the last tenant wrote it, so
        // the scene digest, which hashes whole, agrees between two runs.
        const Run words = mRuns.allocate(runs);
        const Run shares = influences.empty() ? mInfluences.allocateZeroed(1) : mInfluences.allocate(influences);

        return take(Deformer{
            .mKind = Deform::Rig,
            .mRuns = words,
            .mInfluences = shares,
            .mRows = rig.mBones,
        });
    }

    Index DeformerTable::addMorph(const MorphSpec& morph)
    {
        assert(morph.mTargets > 0 && morph.mOffsets.size() % morph.mTargets == 0 && !morph.mOffsets.empty());

        return take(Deformer{
            .mKind = Deform::Morph,
            .mOffsets = mOffsets.allocate(morph.mOffsets),
            .mRows = morph.mTargets,
        });
    }

    bool DeformerTable::pose(const MeshRange& range, const std::span<const PoseWord> words)
    {
        assert(range.mDeform != Deform::None && "a pose for a mesh nothing deforms");

        const Deformer& deformer = mDeformers.at(range.mDeformer);
        assert(deformer.mKind == range.mDeform && "a pose of the other kind than what poses the mesh");

        const std::span<PoseWord> held
            = mPoses.in(Run{ .mOffset = range.mPoseOffset, .mCount = deformer.getPoseWords() });
        assert(words.size() == held.size() && "a pose written over a run of another length");

        // Compared rather than trusted, because the walk poses every rig it meets and cannot know
        // which of them the engine animated.
        if (range.mPosed && std::equal(words.begin(), words.end(), held.begin()))
            return false;

        std::copy(words.begin(), words.end(), held.begin());
        return true;
    }

    std::span<const PoseWord> DeformerTable::getMeshPose(const MeshRange& range) const
    {
        assert(range.mDeform != Deform::None);
        return getPoses().subspan(range.mPoseOffset, mDeformers.at(range.mDeformer).getPoseWords());
    }

    void DeformerTable::release(MeshRange& range)
    {
        if (range.mDeform == Deform::None)
            return;

        mBindRuns.release(Run{ .mOffset = range.mBindOffset, .mCount = range.mVertices.mCount });

        Deformer& deformer = mDeformers.at(range.mDeformer);
        mPoses.release(Run{ .mOffset = range.mPoseOffset, .mCount = deformer.getPoseWords() });

        // The deformer goes with its last mesh, and its runs with it — every run it holds, and
        // an empty one is nothing to release. Nothing downstream is told: what a backend holds of
        // a deformer is data at an offset, read by no frame once no mesh names it, and the next
        // one to land in the run is what names it again.
        if (mDeformers.drop(range.mDeformer))
        {
            mRuns.release(deformer.mRuns);
            mInfluences.release(deformer.mInfluences);
            mOffsets.release(deformer.mOffsets);
            deformer = Deformer{};
            mDeformers.free(range.mDeformer);
            mArrived.remove(range.mDeformer);
        }

        range.mDeform = Deform::None;
        range.mDeformer = sNoIndex;
        range.mPosed = false;
    }

    void DeformerTable::stand(MeshRange& range)
    {
        if (range.mDeform == Deform::None)
            return;

        // A run in the bind table whichever kind it is, because the bind pose is the mesh's
        // vertices and both kinds are computed from them.
        range.mBindOffset = mBindRuns.allocate(range.mVertices.mCount).mOffset;

        // And a run of words, zeroed. Zero is a pose nothing can equal, and `MeshRange::mPosed`
        // is what says the first pose names the mesh regardless.
        mDeformers.hold(range.mDeformer);
        range.mPoseOffset = mPoses.allocateZeroed(mDeformers.at(range.mDeformer).getPoseWords()).mOffset;
    }

    void DeformerTable::compact()
    {
        mArrived.compact();
    }

    void DeformerTable::clearArrivals()
    {
        mArrived.clear();
    }
}
