#include "meshresolver.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <string>

#include <osg/Geometry>

#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>

#include "deformertable.hpp"
#include "error.hpp"
#include "extractionstats.hpp"
#include "instancerecord.hpp"
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

        /// The box a drawable's own bound reaches, in its own space.
        ///
        /// **The sphere and not the box, because that is the one a rig keeps.** `RigGeometry::
        /// updateBounds` writes its sphere straight into the drawable and marks it computed, and
        /// leaves the box to be recomputed from a callback that answers nothing — so asking for the
        /// box would overwrite what the update worked out from the bone spheres. A sphere is a
        /// looser box than the vertices would give, and it is the game's own number: the pose is on
        /// the device and there are no vertices here to walk.
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

    Index MeshResolver::resolve(const osg::Drawable& drawable, const DrawableRead& read, const Index material)
    {
        ExtractionStats& stats = mPass.getStats();

        const osg::Geometry& geometry = *read.mGeometry;

        if (const auto known = mMeshes.find(&drawable); known != mMeshes.end())
        {
            const Index mesh = known->second.mIndex;
            const MeshRange& range = mScene.getTables().mMeshes.getRows()[mesh];

            // Nothing else in the map is re-read: the whole point of it is that a crate met again is
            // the crate already uploaded, and a cell is tens of thousands of these a frame.
            if (read.mDeform == Deform::None && range.mDeform == Deform::None)
            {
                ++stats.mMeshesReused;
                mMeshes.stamp(known);
                return mesh;
            }

            // **What says the slot still fits, and it has to be asked.** The drawable is the same
            // object — the map owns its key, so it cannot be a different one wearing the same
            // address — but a deforming drawable is a shell over a source geometry the engine may
            // replace, and a rig re-pointed at a longer mesh is the same rig. Posing that into the
            // old slot is not a wrong pose: the slot is a run inside one shared vertex buffer, and
            // the kernel would write past it over the meshes that follow.
            //
            // Where the source, the kind or the skin differs the entry is wrong rather than stale,
            // so it goes and the geometry is mirrored afresh. The slot it abandons keeps the epoch
            // it had and the next sweep takes it.
            const std::size_t vertices
                = read.mDeform == Deform::Morph ? morphBase(*read.mMorph).size() : vertexCountOf(geometry);

            const Held held = holdDeformer(read);

            if (vertices == range.mVertices.mCount && read.mDeform == range.mDeform && held.mIndex == range.mDeformer)
            {
                ++stats.mMeshesReused;
                mMeshes.stamp(known);
                stampDeformer(read, held);
                pose(mesh, read, stats);

                return mesh;
            }

            mMeshes.abandon(known);
        }

        MeshReading reading;
        if (!mReader.read(read, reading))
        {
            ++stats.mSkippedEmpty;
            return sNoIndex;
        }

        stats.mFoldMs += reading.mFoldMs;

        if (read.mRig != nullptr && read.mDeform == Deform::None)
            ++stats.mUnskinned;

        if (reading.mShape.mSheet)
            ++stats.mSheets;

        const Index deformer = addDeformer(read, reading.mArrays.mPositions.size());

        const Index mesh = mScene.addMesh(reading.mArrays, reading.mShape, read.mDeform, deformer, material);
        mMeshes.add(&drawable, Known{ .mIndex = mesh });
        ++stats.mMeshesAdded;

        // Posed on arrival as on every frame after: the bind pose the mesh holds is what a pose is
        // computed from, and never what is traced.
        pose(mesh, read, stats);

        return mesh;
    }

    Index MeshResolver::adopt(const osg::Drawable& drawable, const MeshReading& reading, const Index material)
    {
        ExtractionStats& stats = mPass.getStats();

        auto known = mMeshes.find(&drawable);
        if (known != mMeshes.end())
        {
            // A template's drawable is never the walk's: the walk meets clones, and a clone of a
            // deforming drawable is a deep copy at another address. So what the map holds under
            // this key is what this class adopted, and that stands.
            assert(mScene.getTables().mMeshes.getRows()[known->second.mIndex].mDeform == Deform::None
                && "a reading adopted under a drawable the mirror poses");

            ++stats.mMeshesReused;
        }
        else
        {
            if (reading.mShape.mSheet)
                ++stats.mSheets;

            const Index mesh = mScene.addMesh(reading.mArrays, reading.mShape, Deform::None, sNoIndex, material);
            known = mMeshes.add(&drawable, Known{ .mIndex = mesh });
            ++stats.mMeshesAdded;
        }

        mMeshes.hold(known);
        return known->second.mIndex;
    }

    void MeshResolver::release(const osg::Drawable& drawable)
    {
        const auto known = mMeshes.find(&drawable);
        assert(known != mMeshes.end() && "a mesh released that the mirror does not hold");
        mMeshes.drop(known);
    }

    /// Added once per skin and once per set of targets however many drawables share them, and
    /// stamped through `reach` as it goes — so the sweep keeps it for as long as a mesh stands on it.
    Index MeshResolver::addDeformer(const DrawableRead& read, const std::size_t vertices)
    {
        if (read.mDeform == Deform::None)
            return sNoIndex;

        const bool rigged = read.mDeform == Deform::Rig;
        const Index deformer = rigged ? resolveRig(*read.mRig) : resolveMorph(*read.mMorph);
        const DeformerTable& deformers = mScene.getTables().mDeformers;
        const std::size_t skins = rigged ? deformers.getRigs()[deformer].getVertexCount()
                                         : deformers.getMorphs()[deformer].getVertexCount();

        if (skins != vertices)
            throw Error("a deforming mesh of " + std::to_string(vertices) + " vertices on a rig or morph of "
                + std::to_string(skins));

        return deformer;
    }

    MeshResolver::Held MeshResolver::holdDeformer(const DrawableRead& read)
    {
        Held held;

        if (read.mDeform == Deform::Rig)
        {
            held.mRig = mRigs.find(read.mRig->getInfluenceData());
            if (held.mRig != mRigs.end())
                held.mIndex = held.mRig->second.mIndex;
        }
        else if (read.mDeform == Deform::Morph)
        {
            // A morph whose targets changed count under the same base is another morph, so the
            // count is asked beside the identity.
            held.mMorph = mMorphs.find(read.mMorph->getMorphTarget(0).getOffsets());
            if (held.mMorph != mMorphs.end()
                && mScene.getTables().mDeformers.getMorphs()[held.mMorph->second.mIndex].mTargetCount
                    == read.mMorph->getMorphTargetList().size())
                held.mIndex = held.mMorph->second.mIndex;
        }

        return held;
    }

    void MeshResolver::stampDeformer(const DrawableRead& read, const Held& held)
    {
        // **The entry is there, and the fit test is why.** It agreed that the slot's deformer is
        // this drawable's, and neither `resolveRig` nor `resolveMorph` ever hands back `sNoIndex` —
        // so a deformer the sweep had taken would have failed that test rather than reach here.
        if (read.mDeform == Deform::Rig)
        {
            assert(held.mRig != mRigs.end() && "a rigged mesh reused on a skin the mirror has lost");
            mRigs.stamp(held.mRig);
        }
        else if (read.mDeform == Deform::Morph)
        {
            assert(held.mMorph != mMorphs.end() && "a morphed mesh reused on targets the mirror has lost");
            mMorphs.stamp(held.mMorph);
        }
    }

    /// A pose is rows and not vertices, which is why the mirror pays a few dozen matrices for what
    /// is actually moving.
    void MeshResolver::pose(const Index mesh, const DrawableRead& read, ExtractionStats& stats)
    {
        if (read.mDeform == Deform::None)
            return;

        if (read.mDeform == Deform::Rig)
            poseRig(mesh, *read.mRig);
        else
            poseMorph(mesh, *read.mMorph);

        ++stats.mDeformed;
    }

    Index MeshResolver::resolveRig(const SceneUtil::RigGeometry& rig)
    {
        const SceneUtil::RigGeometry::InfluenceData* skin = rig.getInfluenceData();
        assert(skin != nullptr);

        const std::size_t vertices = vertexCountOf(*rig.getSourceGeometry());

        // **A skin rewritten in place under the same address is a new skin.** `setInfluences` on a
        // rig the mirror has met writes into the `InfluenceData` every copy shares, so what the map
        // holds describes a mesh of another length; the rig it named stays for the meshes still on
        // it and goes with the last of them, and this drawable gets one of its own.
        const auto [known, arrived] = mRigs.reach(skin);
        if (!arrived && mScene.getTables().mDeformers.getRigs()[known->second.mIndex].getVertexCount() == vertices)
            return known->second.mIndex;

        // **The groups flattened into a run per vertex.** `RigGeometry::setInfluences` gathers the
        // vertices that share one weight list so the rasterizer blends each list once; a kernel
        // blends per lane and wants to find its list from its vertex, which is what the run word
        // is. A vertex in no group is a run of nothing, as the rasterizer leaves it at the origin.

        mRunScratch.assign(vertices, 0);
        mInfluenceScratch.clear();
        for (const auto& [weights, group] : skin->mInfluences)
        {
            if (weights.size() > Shaders::RUN_COUNT_MASK)
                throw Error("a vertex skinned by " + std::to_string(weights.size()) + " bones, past the "
                    + std::to_string(Shaders::RUN_COUNT_MASK) + " a run word holds");

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
                    throw Error("a skin naming vertex " + std::to_string(vertex) + " of a mesh with "
                        + std::to_string(vertices));

                mRunScratch[vertex] = run;
            }
        }

        known->second.mIndex = mScene.addRig(mRunScratch, mInfluenceScratch, static_cast<Index>(skin->mBones.size()));
        return known->second.mIndex;
    }

    Index MeshResolver::resolveMorph(const SceneUtil::MorphGeometry& morph)
    {
        const SceneUtil::MorphGeometry::MorphTargetList& targets = morph.getMorphTargetList();
        assert(targets.size() > 1);

        const std::size_t vertices = morphBase(morph).size();

        // A set of targets grown or shrunk under the same base is a new set, for the reason a
        // rewritten skin is a new skin.
        const auto [known, arrived] = mMorphs.reach(targets[0].getOffsets());
        if (!arrived)
        {
            const Morph& held = mScene.getTables().mDeformers.getMorphs()[known->second.mIndex];
            if (held.mTargetCount == targets.size() && held.getVertexCount() == vertices)
                return known->second.mIndex;
        }

        // Every target's offsets laid end to end, the base's included as a run of zeroes so the
        // table's target `k` is the drawable's target `k` and a weight indexes both the same way.
        // `MorphGeometry::cull` reads target `k` as `offsets[k][vertex]` for every `k` past the
        // base; a target shorter than the base is read as far as it goes and the rest left alone,
        // which a zero past its end is.
        mOffsetScratch.assign(vertices * targets.size(), osg::Vec3f());
        for (std::size_t target = 1; target < targets.size(); ++target)
        {
            const osg::Vec3Array* offsets = targets[target].getOffsets();
            if (offsets == nullptr)
                continue;

            const std::size_t count = std::min<std::size_t>(offsets->size(), vertices);
            std::copy_n(offsets->begin(), count, mOffsetScratch.begin() + target * vertices);
        }

        known->second.mIndex = mScene.addMorph(mOffsetScratch, static_cast<Index>(targets.size()));
        return known->second.mIndex;
    }

    void MeshResolver::poseRig(Index mesh, const SceneUtil::RigGeometry& rig)
    {
        const SceneUtil::RigGeometry::InfluenceData& skin = *rig.getInfluenceData();
        const std::span<SceneUtil::Bone* const> bones = rig.getBones();
        assert(bones.size() == skin.mBones.size());

        // `RigGeometry::cull`'s arithmetic, row for row: each bone's inverse bind by its
        // skeleton-space matrix, and the skin's transform after the blend — composed into every
        // bone here, which is the same product because the blend is linear and the transform is
        // affine. A bone the skeleton has not got contributes nothing, as it does there.
        //
        // **From the matrices the update traversal left.** `RigGeometry::updateBounds` runs
        // `Skeleton::updateBoneMatrices` under it for every active skeleton and on the first frame
        // regardless, and a skeleton it skipped is one whose bones did not move — so what is here
        // is this frame's pose or the last one, and either is what the rasterizer would show.
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

        mScene.poseRig(mesh, mBoneScratch, reachOf(rig));
    }

    void MeshResolver::poseMorph(Index mesh, const SceneUtil::MorphGeometry& morph)
    {
        const SceneUtil::MorphGeometry::MorphTargetList& targets = morph.getMorphTargetList();

        mWeightScratch.clear();
        mWeightScratch.reserve(targets.size());
        for (const SceneUtil::MorphGeometry::MorphTarget& target : targets)
            mWeightScratch.push_back(target.getWeight());

        mScene.poseMorph(mesh, mWeightScratch, reachOf(morph));
    }

    void MeshResolver::retire(std::vector<Index>& live)
    {
        mMeshes.sweep(live);
    }

    void MeshResolver::retireDeformers()
    {
        // **A rig and a morph are swept on the meshes' stamp and not on a use count of their own.**
        // Each is shared by every drawable that carries it, so what says one is gone is that no mesh
        // named it this epoch — which the scene decides for itself by counting uses. What is swept
        // here is only this mirror's hold on the data, and the two agree because a rig is stamped
        // exactly where a mesh on it is met.
        mRigs.retire();
        mMorphs.retire();
    }
}
