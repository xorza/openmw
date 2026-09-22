#include "meshresolver.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <osg/Array>
#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/Geometry>
#include <osg/Matrix>
#include <osg/Matrixf>

#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>

#include "contract.hpp"
#include "deformertable.hpp"
#include "extractionstats.hpp"
#include "instancerecord.hpp"
#include "mesh.hpp"
#include "refusals.hpp"
#include "result.hpp"
#include "scenedesc.hpp"

namespace Rtx
{
    namespace
    {
        /// How many vertices a geometry has, or nought where it holds none it can be read for.
        /// Asked on its own where the count is the whole question, so a body met again does not
        /// spread its normals to find out.
        std::size_t vertexCountOf(const osg::Geometry& geometry)
        {
            const osg::Array* positions = geometry.getVertexArray();
            return positions != nullptr && positions->getType() == osg::Array::Vec3ArrayType
                ? positions->getNumElements()
                : 0;
        }

        /// The box a drawable's own bound reaches, in its own space — off the sphere, because
        /// `RigGeometry::updateBounds` writes its sphere straight into the drawable and asking for
        /// the box would overwrite what it worked out from the bone spheres.
        osg::BoundingBoxf reachOf(const osg::Drawable& drawable)
        {
            const osg::BoundingSphere& sphere = drawable.getBound();
            if (!sphere.valid())
                return osg::BoundingBoxf();

            const osg::Vec3f centre(sphere.center());
            const osg::Vec3f reach(sphere.radius(), sphere.radius(), sphere.radius());
            return osg::BoundingBoxf(centre - reach, centre + reach);
        }

    }

    Index MeshResolver::resolve(const osg::Drawable& drawable, const DrawableRead& read)
    {
        ExtractionStats& stats = mPass.getStats();

        const osg::Geometry& geometry = *read.mGeometry;

        if (const auto known = mMeshes.find(&drawable); known != mMeshes.end())
        {
            const Index mesh = known->second.mIndex;

            // Refused once, and never read again: the file has not changed since.
            if (mesh == sNoIndex)
            {
                mMeshes.stamp(known);
                return sNoIndex;
            }

            const MeshRange& range = mScene.meshes().getRows()[mesh];

            // Nothing else in the map is re-read: the whole point of it is that a crate met again is
            // the crate already uploaded, and a cell is tens of thousands of these a frame.
            if (read.mDeform == Deform::None && range.mDeform == Deform::None)
            {
                ++stats.mMeshesReused;
                mMeshes.stamp(known);
                return mesh;
            }

            // What says the slot still fits, and it has to be asked: a deforming drawable is a
            // shell over a source geometry the engine may replace, and a rig re-pointed at a longer
            // mesh posed into the old slot would write past it over the meshes that follow. Where
            // the source, the kind or the skin differs the entry goes and the geometry is mirrored
            // afresh.
            const std::size_t vertices
                = read.mDeform == Deform::Morph ? morphBase(*read.mMorph).size() : vertexCountOf(geometry);

            const Held held = holdDeformer(read);

            if (vertices == range.mVertices.mCount && read.mDeform == range.mDeform && held.mIndex == range.mDeformer)
            {
                ++stats.mMeshesReused;
                mMeshes.stamp(known);
                stampDeformer(held);
                pose(mesh, read, stats);

                return mesh;
            }

            mMeshes.abandon(known);
        }

        // **What a content file says is refused here, per drawable, and the frame goes on.** Every
        // check runs before a row is made, so a refusal leaves the scene as it was. The refusal is
        // kept under the drawable, stamped as the walk meets it, so a file the renderer cannot
        // take is read once for as long as it stands rather than once a frame.
        MeshReading reading;
        const Result<bool, std::string> readMesh = mReader.read(read, reading);
        if (!readMesh.isOk())
            return refuse(drawable, readMesh.error());

        if (!readMesh.value())
        {
            ++stats.mSkippedEmpty;
            return sNoIndex;
        }

        const Result<Index, std::string> added = addMesh(read, reading);
        if (!added.isOk())
            return refuse(drawable, added.error());

        const Index mesh = added.value();

        stats.mFoldMs += reading.mFoldMs;

        if (read.mRig != nullptr && read.mDeform == Deform::None)
            ++stats.mUnskinned;

        if (reading.mShape.mSheet)
            ++stats.mSheets;

        mMeshes.add(&drawable, Known{ .mIndex = mesh });
        ++stats.mMeshesAdded;

        // Posed on arrival as on every frame after: the bind pose the mesh holds is what a pose is
        // computed from, and never what is traced.
        pose(mesh, read, stats);

        return mesh;
    }

    const osg::Referenced* MeshResolver::deformerKeyOf(const DrawableRead& read)
    {
        switch (read.mDeform)
        {
            case Deform::Rig:
                return read.mRig->getInfluenceData();
            case Deform::Morph:
                return read.mMorph->getMorphTarget(0).getOffsets();
            case Deform::None:
                break;
        }

        return nullptr;
    }

    bool MeshResolver::fits(const Index slot, const DrawableRead& read) const
    {
        const Deformer& held = mScene.deformers().getDeformers()[slot];
        if (held.mKind != read.mDeform)
            return false;

        return read.mDeform != Deform::Morph || held.mRows == read.mMorph->getMorphTargetList().size();
    }

    Index MeshResolver::adopt(const osg::Drawable& drawable, const MeshReading& reading)
    {
        ExtractionStats& stats = mPass.getStats();

        auto known = mMeshes.find(&drawable);
        if (known != mMeshes.end())
        {
            // A template's drawable is never the walk's: the walk meets clones, and a clone of a
            // deforming drawable is a deep copy at another address. So what the map holds under
            // this key is what this class adopted, and that stands.
            assert(mScene.meshes().getRows()[known->second.mIndex].mDeform == Deform::None
                && "a reading adopted under a drawable the mirror poses");

            ++stats.mMeshesReused;
        }
        else
        {
            if (reading.mShape.mSheet)
                ++stats.mSheets;

            const Index mesh = mScene.addMesh(reading.mArrays, reading.mShape, Deform::None, sNoIndex);
            known = mMeshes.add(&drawable, Known{ .mIndex = mesh });
            ++stats.mMeshesAdded;
        }

        mMeshes.hold(known);
        return known->second.mIndex;
    }

    void MeshResolver::release(const osg::Drawable& drawable)
    {
        const auto known = mMeshes.find(&drawable);
        contract(known != mMeshes.end(), "a mesh released that the mirror does not hold");
        mMeshes.drop(known);
    }

    Index MeshResolver::refuse(const osg::Drawable& drawable, std::string_view why)
    {
        mScene.refusals().refuse(Refused::Mesh, drawable.getName(), why);
        mMeshes.add(&drawable, Known{ .mIndex = sNoIndex });
        return sNoIndex;
    }

    Result<Index, std::string> MeshResolver::addMesh(const DrawableRead& read, const MeshReading& reading)
    {
        if (const Result<void, std::string> fits = MeshTable::checkFits(reading.mArrays); !fits.isOk())
            return Err{ fits.error() };

        if (read.mDeform == Deform::None)
            return mScene.addMesh(reading.mArrays, reading.mShape, Deform::None, sNoIndex);

        // A deformer the mirror holds that fits this drawable — the same kind, the same targets
        // and exactly these vertices — is added once per skin and once per set of targets however
        // many drawables share them, and stamped as each is met, so the sweep keeps it for as long
        // as a mesh stands on it.
        const std::size_t vertices = reading.mArrays.mPositions.size();
        const Held held = holdDeformer(read);
        if (held.mIndex != sNoIndex && mScene.deformers().getDeformers()[held.mIndex].getVertexCount() == vertices)
        {
            stampDeformer(held);
            return mScene.addMesh(reading.mArrays, reading.mShape, read.mDeform, held.mIndex);
        }

        // Otherwise this drawable gets a deformer of its own, made with its mesh. A skin rewritten
        // in place under the same address is a new skin: `setInfluences` on a rig the mirror has
        // met writes into the `InfluenceData` every copy shares, so what the map held described a
        // mesh of another length; the deformer it named stays for the meshes still on it and goes
        // with the last of them. A set of targets grown or shrunk under the same base is a new set
        // for the same reason.
        DeformedMesh added;
        if (read.mDeform == Deform::Rig)
        {
            const Result<RigSpec, std::string> rig = readRig(*read.mRig);
            if (!rig.isOk())
                return Err{ rig.error() };

            const Result<void, std::string> posed
                = SceneDesc::checkPoses(rig.value().getVertexCount(), reading.mArrays);
            if (!posed.isOk())
                return Err{ posed.error() };

            added = mScene.addMesh(reading.mArrays, reading.mShape, rig.value());
        }
        else
        {
            const MorphSpec morph = readMorph(*read.mMorph);
            const Result<void, std::string> posed = SceneDesc::checkPoses(morph.getVertexCount(), reading.mArrays);
            if (!posed.isOk())
                return Err{ posed.error() };

            added = mScene.addMesh(reading.mArrays, reading.mShape, morph);
        }

        if (held.mEntry != mDeformers.end())
        {
            held.mEntry->second.mIndex = added.mDeformer;
            mDeformers.stamp(held.mEntry);
        }
        else
            mDeformers.add(deformerKeyOf(read), Known{ .mIndex = added.mDeformer });

        return added.mMesh;
    }

    MeshResolver::Held MeshResolver::holdDeformer(const DrawableRead& read)
    {
        Held held;
        if (read.mDeform == Deform::None)
            return held;

        held.mEntry = mDeformers.find(deformerKeyOf(read));
        if (held.mEntry != mDeformers.end() && fits(held.mEntry->second.mIndex, read))
            held.mIndex = held.mEntry->second.mIndex;

        return held;
    }

    void MeshResolver::stampDeformer(const Held& held)
    {
        // The entry is there, and the fit test is why. It agreed that the slot's deformer is
        // this drawable's, and an entry names a deformer from the moment it is made (`addMesh`
        // makes the rows first) — so a deformer the sweep had taken would have failed that test
        // rather than reach here.
        contract(held.mEntry != mDeformers.end(), "a deforming mesh reused on a deformer the mirror has lost");
        mDeformers.stamp(held.mEntry);
    }

    /// A pose is rows and not vertices, which is why the mirror pays a few dozen matrices for what
    /// is actually moving.
    void MeshResolver::pose(const Index mesh, const DrawableRead& read, ExtractionStats& stats)
    {
        if (read.mDeform == Deform::None)
            return;

        if (read.mDeform == Deform::Rig)
        {
            const SceneUtil::RigGeometry& rig = *read.mRig;
            const SceneUtil::RigGeometry::InfluenceData& skin = *rig.getInfluenceData();
            const std::span<SceneUtil::Bone* const> bones = rig.getBones();
            assert(bones.size() == skin.mBones.size());

            // `RigGeometry::cull`'s arithmetic, row for row, with the skin's transform composed
            // into every bone, which is the same product because the blend is linear and the
            // transform affine. From the matrices the update traversal left: a skeleton it
            // skipped is one whose bones did not move.
            osg::Matrixf transform = skin.mTransform;
            if (const osg::RefMatrix* skinToSkel = rig.getSkinToSkelMatrix())
                transform = (*skinToSkel) * skin.mTransform;

            mBoneScratch.clear();
            mBoneScratch.reserve(bones.size());
            for (std::size_t at = 0; at < bones.size(); ++at)
            {
                if (bones[at] == nullptr)
                {
                    mBoneScratch.push_back(Shaders::GpuBone{});
                    continue;
                }

                mBoneScratch.push_back(
                    toGpuBone(skin.mBones[at].mInvBindMatrix * bones[at]->mMatrixInSkeletonSpace * transform));
            }

            packBones(mBoneScratch, mPoseScratch);
            mScene.pose(mesh, mPoseScratch, reachOf(rig));
        }
        else
        {
            const SceneUtil::MorphGeometry& morph = *read.mMorph;
            const SceneUtil::MorphGeometry::MorphTargetList& targets = morph.getMorphTargetList();

            mWeightScratch.clear();
            mWeightScratch.reserve(targets.size());
            for (const SceneUtil::MorphGeometry::MorphTarget& target : targets)
                mWeightScratch.push_back(target.getWeight());

            packWeights(mWeightScratch, mPoseScratch);
            mScene.pose(mesh, mPoseScratch, reachOf(morph));
        }

        ++stats.mDeformed;
    }

    Result<RigSpec, std::string> MeshResolver::readRig(const SceneUtil::RigGeometry& rig)
    {
        const SceneUtil::RigGeometry::InfluenceData* skin = rig.getInfluenceData();
        assert(skin != nullptr);

        const std::size_t vertices = vertexCountOf(*rig.getSourceGeometry());

        // The groups flattened into a run per vertex. `RigGeometry::setInfluences` gathers the
        // vertices that share one weight list so the rasterizer blends each list once; a kernel
        // blends per lane and wants to find its list from its vertex, which is what the run word
        // is. A vertex in no group is a run of nothing, as the rasterizer leaves it at the origin.

        mRunScratch.assign(vertices, 0);
        mInfluenceScratch.clear();
        for (const auto& [weights, group] : skin->mInfluences)
        {
            if (weights.size() > Shaders::RUN_COUNT_MASK)
                return Err{ "a vertex of it is skinned by " + std::to_string(weights.size()) + " bones, past the "
                    + std::to_string(Shaders::RUN_COUNT_MASK) + " a run word holds" };

            const auto first = static_cast<std::uint32_t>(mInfluenceScratch.size());
            for (const auto& [bone, weight] : weights)
                mInfluenceScratch.push_back(Shaders::GpuInfluence{
                    .mBone = static_cast<std::uint32_t>(bone),
                    .mWeight = weight,
                });

            const std::uint32_t run = (first << Shaders::RUN_COUNT_BITS) | static_cast<std::uint32_t>(weights.size());
            for (const unsigned short vertex : group)
            {
                if (vertex >= vertices)
                    return Err{ "its skin names vertex " + std::to_string(vertex) + " of " + std::to_string(vertices) };

                mRunScratch[vertex] = run;
            }
        }

        return RigSpec{
            .mRuns = mRunScratch,
            .mInfluences = mInfluenceScratch,
            .mBones = static_cast<Index>(skin->mBones.size()),
        };
    }

    MorphSpec MeshResolver::readMorph(const SceneUtil::MorphGeometry& morph)
    {
        const SceneUtil::MorphGeometry::MorphTargetList& targets = morph.getMorphTargetList();
        assert(targets.size() > 1);

        const std::size_t vertices = morphBase(morph).size();

        // Every target's offsets laid end to end, the base's included as a run of zeroes so a
        // weight indexes the table and the drawable the same way. A target shorter than the base
        // is read as far as it goes, which a zero past its end is.
        mOffsetScratch.assign(vertices * targets.size(), osg::Vec3f());
        for (std::size_t target = 1; target < targets.size(); ++target)
        {
            const osg::Vec3Array* offsets = targets[target].getOffsets();
            if (offsets == nullptr)
                continue;

            const std::size_t count = std::min<std::size_t>(offsets->size(), vertices);
            std::copy_n(offsets->begin(), count, mOffsetScratch.begin() + target * vertices);
        }

        return MorphSpec{ .mOffsets = mOffsetScratch, .mTargets = static_cast<Index>(targets.size()) };
    }

    void MeshResolver::retire(std::vector<Index>& live)
    {
        mMeshes.sweep(live);
    }

    void MeshResolver::retireDeformers()
    {
        // A deformer is swept on the meshes' stamp and not on a use count of its own; the scene
        // counts uses for itself, and the two agree because a deformer is stamped exactly where a
        // mesh on it is met.
        mDeformers.retire();
    }
}
