#include "meshresolver.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <string>

#include <osg/Geometry>
#include <osg/TriangleIndexFunctor>

#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>

#include "error.hpp"
#include "extractionstats.hpp"
#include "instancerecord.hpp"
#include "scenedesc.hpp"

namespace Rtx
{
    namespace
    {
        /// Collects triangle indices whatever primitive mode the geometry used.
        ///
        /// Strips, fans and quads all arrive here as triangles, which is the only form an
        /// acceleration structure takes. Degenerate triangles — how a strip restarts — are dropped:
        /// they contribute no surface and a zero-area triangle in a BLAS is wasted traversal.
        struct TriangleCollector
        {
            std::vector<std::uint32_t>* mIndices = nullptr;

            void operator()(unsigned int a, unsigned int b, unsigned int c) const
            {
                if (a == b || b == c || a == c)
                    return;

                mIndices->push_back(a);
                mIndices->push_back(b);
                mIndices->push_back(c);
            }
        };

        /// A geometry's per-vertex positions and normals.
        struct VertexArrays
        {
            std::span<const osg::Vec3f> mPositions;

            /// Empty only where the geometry names no normal at all. A per-vertex array is taken as
            /// it stands and a single overall one is spread across the vertices, which is the same
            /// answer at every point of a flat surface.
            std::span<const osg::Vec3f> mNormals;
        };

        /// The array as a `Vec3Array`, or null where it is anything else.
        ///
        /// **`osg::Array` states its own type in a byte**, which is what a `dynamic_cast` walks the
        /// class hierarchy to work out — the same shape as the library-name test the walk already
        /// makes of a drawable and of a terrain chunk before it casts either. `vertexCountOf` asks
        /// it of every posed part every frame; the two in `readVertices` are on the arrival path.
        const osg::Vec3Array* asVec3Array(const osg::Array* array)
        {
            if (array == nullptr || array->getType() != osg::Array::Vec3ArrayType)
                return nullptr;

            return static_cast<const osg::Vec3Array*>(array);
        }

        /// @param flat scratch for an overall normal spread across the vertices. Refilled here and
        ///        borrowed by the returned span, so it has to outlive the read.
        VertexArrays readVertices(const osg::Geometry& geometry, std::vector<osg::Vec3f>& flat)
        {
            VertexArrays arrays;

            const osg::Vec3Array* positions = asVec3Array(geometry.getVertexArray());
            if (positions == nullptr)
                return arrays;

            arrays.mPositions = std::span(positions->asVector());

            const osg::Vec3Array* normals = asVec3Array(geometry.getNormalArray());
            if (normals == nullptr || normals->empty())
                return arrays;

            if (normals->size() == positions->size())
            {
                arrays.mNormals = std::span(normals->asVector());
                return arrays;
            }

            // **One normal for the whole drawable is a normal, and dropping it made the sea flat
            // black.** `SceneUtil::createWaterGeometry` binds exactly this — a thousand vertices and
            // one `(0, 0, 1)` — so the game's water mirrored with no normal at all, and shading a
            // surface by a zero vector produces radiance that the frame's own exposure then reads.
            // Everything else in the picture goes with it.
            if (normals->getBinding() != osg::Array::BIND_OVERALL)
                return arrays;

            flat.assign(positions->size(), normals->at(0));
            arrays.mNormals = std::span(flat);
            return arrays;
        }

        /// How many vertices a geometry has, or nought where it holds none it can be read for.
        /// Asked on its own where the count is the whole question, so a body met again does not
        /// spread its normals to find out.
        std::size_t vertexCountOf(const osg::Geometry& geometry)
        {
            const osg::Vec3Array* positions = asVec3Array(geometry.getVertexArray());
            return positions != nullptr ? positions->size() : 0;
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

        /// A morph's base target, which `MorphGeometry::cull` reads its positions from. The source
        /// geometry's own array is what `NifOsg` built the drawable from and the two agree in every
        /// file it builds, so the length is asserted and the base is what is read.
        std::span<const osg::Vec3f> baseOf(const SceneUtil::MorphGeometry& morph)
        {
            const osg::Vec3Array* base = morph.getMorphTarget(0).getOffsets();
            assert(base != nullptr && "a morph whose base is no array");
            return std::span(base->asVector());
        }
    }

    /// Nearly everything in a cell is an `osg::Geometry` and answers in one virtual call. A skinned
    /// body and a morphed face are not: each is an `osg::Drawable` over a source geometry, and the
    /// source is what this reads — the bind pose a skin is computed from, the base a morph starts
    /// from. **Not the double-buffered copy a cull traversal writes**, which no walk of this
    /// renderer runs any more: the pose is bone rows and weights handed to the device, and the
    /// device computes the vertices where the structure is refitted from them.
    ///
    /// **A rig no update traversal has resolved is read as it stands.** Its bones are what
    /// `RigGeometry::updateBounds` finds under the update traversal, and a rig with none has nothing
    /// to be posed against; the rasterizer draws that rig in its bind pose, and so does this. A morph
    /// with no target past its base has nothing to move either, and is a static mesh whose
    /// positions are the base.
    MeshResolver::Read MeshResolver::readDrawable(const osg::Drawable& drawable)
    {
        if (const osg::Geometry* geometry = drawable.asGeometry())
            return Read{ .mGeometry = geometry };

        if (const auto* rig = dynamic_cast<const SceneUtil::RigGeometry*>(&drawable))
        {
            const bool skinned = rig->getInfluenceData() != nullptr && !rig->getBones().empty();
            return Read{ .mGeometry = rig->getSourceGeometry().get(),
                .mDeform = skinned ? Deform::Rig : Deform::None,
                .mRig = rig };
        }

        if (const auto* morph = dynamic_cast<const SceneUtil::MorphGeometry*>(&drawable))
        {
            const bool moving = morph->getMorphTargetList().size() > 1;
            return Read{ .mGeometry = morph->getSourceGeometry().get(),
                .mDeform = moving ? Deform::Morph : Deform::None,
                .mMorph = morph };
        }

        return Read{};
    }

    Index MeshResolver::resolve(const osg::Drawable& drawable, const Read& read, const Index material)
    {
        ExtractionStats& stats = mPass.getStats();

        const osg::Geometry& geometry = *read.mGeometry;

        if (const auto known = mMeshes.find(&drawable); known != mMeshes.end())
        {
            const Index mesh = known->second.mIndex;
            const MeshRange& range = mScene.getMeshes()[mesh];

            // Nothing else in the map is re-read: the whole point of it is that a crate met again is
            // the crate already uploaded, and a cell is tens of thousands of these a frame.
            if (read.mDeform == Deform::None && range.mDeform == Deform::None)
            {
                ++stats.mMeshesReused;
                known->second.mEpoch = mPass.mEpoch;
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
                = read.mDeform == Deform::Morph ? baseOf(*read.mMorph).size() : vertexCountOf(geometry);

            // What the drawable's skin or targets resolve to, where the mirror has met them, and
            // `sNoIndex` where it has not or where the drawable stands — which is what a slot that
            // stands holds too. A morph whose targets changed count under the same base is another
            // morph, so the count is asked beside the identity.
            //
            // The entry each is found in is kept, because the stamp below wants the same one: a
            // second `find` per posed part per frame is a hash of a pointer and a bucket walk for a
            // question already answered, and Vivec poses 332.
            Index deformer = sNoIndex;
            auto rig = mRigs.end();
            auto morph = mMorphs.end();
            if (read.mDeform == Deform::Rig)
            {
                rig = mRigs.find(read.mRig->getInfluenceData());
                if (rig != mRigs.end())
                    deformer = rig->second.mIndex;
            }
            else if (read.mDeform == Deform::Morph)
            {
                morph = mMorphs.find(read.mMorph->getMorphTarget(0).getOffsets());
                if (morph != mMorphs.end()
                    && mScene.getMorphs()[morph->second.mIndex].mTargetCount
                        == read.mMorph->getMorphTargetList().size())
                    deformer = morph->second.mIndex;
            }

            if (vertices == range.mVertexCount && read.mDeform == range.mDeform && deformer == range.mDeformer)
            {
                ++stats.mMeshesReused;
                known->second.mEpoch = mPass.mEpoch;

                // A pose is rows and not vertices, which is why the mirror pays a few dozen
                // matrices for what is actually moving. The skin is stamped with the mesh, which is
                // what keeps the sweep's two answers one answer.
                //
                // The entry found above is what stamps it, and it is there: the slot agrees with
                // this drawable's deformer, and neither `resolveRig` nor `resolveMorph` ever hands
                // back `sNoIndex` — so a deformer the sweep took would have failed the test above
                // rather than reach here.
                if (read.mDeform == Deform::Rig)
                {
                    assert(rig != mRigs.end() && "a rigged mesh reused on a skin the mirror has lost");
                    rig->second.mEpoch = mPass.mEpoch;
                    poseRig(mesh, *read.mRig);
                    ++stats.mDeformed;
                }
                else if (read.mDeform == Deform::Morph)
                {
                    assert(morph != mMorphs.end() && "a morphed mesh reused on targets the mirror has lost");
                    morph->second.mEpoch = mPass.mEpoch;
                    poseMorph(mesh, *read.mMorph);
                    ++stats.mDeformed;
                }

                return mesh;
            }

            mMeshes.erase(known);
        }

        VertexArrays arrays = readVertices(geometry, mFlatNormalScratch);

        // A morph starts from its base target and not from the source's array, because that is
        // what `MorphGeometry::cull` starts from. The normals and everything else are the source's.
        if (read.mDeform == Deform::Morph)
        {
            const std::span<const osg::Vec3f> base = baseOf(*read.mMorph);
            if (base.size() != arrays.mPositions.size())
                throw Error("a morphed face of " + std::to_string(arrays.mPositions.size())
                    + " vertices whose base target has " + std::to_string(base.size()));

            arrays.mPositions = base;
        }

        if (arrays.mPositions.empty())
        {
            ++stats.mSkippedEmpty;
            return sNoIndex;
        }

        if (read.mRig != nullptr && read.mDeform == Deform::None)
            ++stats.mUnskinned;

        mIndexScratch.clear();
        osg::TriangleIndexFunctor<TriangleCollector> collector;
        collector.mIndices = &mIndexScratch;
        geometry.accept(collector);

        if (mIndexScratch.empty())
        {
            ++stats.mSkippedEmpty;
            return sNoIndex;
        }

        std::span<const osg::Vec2f> texCoords;
        const auto* texCoordArray = dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(0));
        if (texCoordArray != nullptr && texCoordArray->size() == arrays.mPositions.size())
            texCoords = std::span(texCoordArray->asVector());

        // Before the mesh is written, so the copy the content drew for a card's back never reaches
        // a structure. Once per drawable and never for a pose: a rig moves the two copies together,
        // so the pairs found in the bind pose are the pairs.
        const FoldedShape shape = mShapeFold.fold(arrays.mPositions, mIndexScratch);
        if (shape.mSheet)
            ++stats.mSheets;

        // What poses it, added once per skin and once per set of targets however many drawables
        // share them, and stamped here so the sweep keeps it for as long as a mesh stands on it.
        Index deformer = sNoIndex;
        if (read.mDeform == Deform::Rig)
            deformer = resolveRig(*read.mRig);
        else if (read.mDeform == Deform::Morph)
            deformer = resolveMorph(*read.mMorph);

        if (deformer != sNoIndex)
        {
            const Index skinned = read.mDeform == Deform::Rig ? mScene.getRigs()[deformer].mVertexCount
                                                              : mScene.getMorphs()[deformer].mVertexCount;
            if (skinned != arrays.mPositions.size())
                throw Error("a deforming mesh of " + std::to_string(arrays.mPositions.size())
                    + " vertices on a rig or morph of " + std::to_string(skinned));
        }

        const Index mesh = mScene.addMesh(
            arrays.mPositions, arrays.mNormals, texCoords, mIndexScratch, shape, read.mDeform, deformer, material);
        mMeshes.emplace(&drawable, Known{ .mIndex = mesh, .mEpoch = mPass.mEpoch });
        ++stats.mMeshesAdded;

        // Posed on arrival as on every frame after: the bind pose the mesh holds is what a pose is
        // computed from, and never what is traced.
        if (read.mDeform == Deform::Rig)
        {
            poseRig(mesh, *read.mRig);
            ++stats.mDeformed;
        }
        else if (read.mDeform == Deform::Morph)
        {
            poseMorph(mesh, *read.mMorph);
            ++stats.mDeformed;
        }

        return mesh;
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
        auto [known, arrived] = mRigs.try_emplace(skin);
        known->second.mEpoch = mPass.mEpoch;
        if (!arrived && mScene.getRigs()[known->second.mIndex].mVertexCount == vertices)
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

        const std::size_t vertices = baseOf(morph).size();

        // A set of targets grown or shrunk under the same base is a new set, for the reason a
        // rewritten skin is a new skin.
        auto [known, arrived] = mMorphs.try_emplace(targets[0].getOffsets());
        known->second.mEpoch = mPass.mEpoch;
        if (!arrived)
        {
            const Morph& held = mScene.getMorphs()[known->second.mIndex];
            if (held.mTargetCount == targets.size() && held.mVertexCount == vertices)
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

    std::uint32_t MeshResolver::retire(std::vector<Index>& live)
    {
        const std::uint32_t went = sweep(mMeshes, mPass.mEpoch, live);

        // **A rig and a morph are swept beside the meshes and not by their own count.** Each is
        // shared by every drawable that carries it, so what says one is gone is that no mesh named
        // it this epoch — which the scene decides for itself by counting uses. What is swept here is
        // only this mirror's hold on the data, and the two agree because a rig is stamped exactly
        // where a mesh on it is met.
        std::erase_if(mRigs, [this](const auto& entry) { return entry.second.mEpoch != mPass.mEpoch; });
        std::erase_if(mMorphs, [this](const auto& entry) { return entry.second.mEpoch != mPass.mEpoch; });

        return went;
    }
}
