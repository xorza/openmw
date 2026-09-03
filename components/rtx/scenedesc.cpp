#include "scenedesc.hpp"

#include <algorithm>
#include <cassert>
#include <tuple>

#include "error.hpp"

namespace Rtx
{
    namespace
    {
        /// What a blended material is tested against when it named no threshold of its own.
        ///
        /// Half, because the alpha it is standing in for is very nearly binary already: Morrowind's
        /// masks are painted, not anti-aliased, and the fringe a filter puts on them is a texel wide.
        constexpr float sBlendCutoff = 0.5f;

        /// The box every one of `positions` fits in.
        osg::BoundingBoxf boundsOf(std::span<const osg::Vec3f> positions)
        {
            osg::BoundingBoxf bounds;
            for (const osg::Vec3f& position : positions)
                bounds.expandBy(position);

            return bounds;
        }

        /// Puts `row` in a slot of `table` nothing stands in, or on the end where there is none, and
        /// says which.
        ///
        /// **Any free slot will do.** A slot is one row of a table and every row is the same size;
        /// what varies in length — the geometry, the layers, the runs — the allocators have already
        /// placed. Taken from the back, because there is no fit to find.
        template <class Row>
        Index takeSlot(std::vector<Row>& table, std::vector<Index>& free, const Row& row)
        {
            if (!free.empty())
            {
                const Index index = free.back();
                free.pop_back();
                table[index] = row;
                return index;
            }

            table.push_back(row);
            return static_cast<Index>(table.size() - 1);
        }
    }

    float Material::getAlphaCutoff() const
    {
        switch (mAlphaMode)
        {
            case AlphaMode::Opaque:
                return 0.0f;
            case AlphaMode::Cutout:
                return mAlphaRef;
            case AlphaMode::Blend:
                return mAlphaRef > 0.0f ? mAlphaRef : sBlendCutoff;
        }

        return 0.0f;
    }

    Index SceneDesc::addMesh(std::span<const osg::Vec3f> positions, std::span<const osg::Vec3f> normals,
        std::span<const osg::Vec2f> texCoords, std::span<const std::uint32_t> indices, FoldedShape shape, Deform deform,
        Index deformer, Index material)
    {
        assert(!positions.empty());
        assert((material == sNoIndex || material < mMaterials.size()) && "a mesh wearing a material the scene lacks");
        assert(normals.empty() || normals.size() == positions.size());
        assert(texCoords.empty() || texCoords.size() == positions.size());
        assert(indices.size() % 3 == 0);
        assert(std::all_of(indices.begin(), indices.end(), [&](std::uint32_t i) { return i < positions.size(); }));
        assert((deform == Deform::None) == (deformer == sNoIndex) && "a deforming mesh names what poses it");
        assert(deform != Deform::Rig
            || (deformer < mRigs.size() && mRigs[deformer].mVertexCount == positions.size()
                && "a rig skins exactly the vertices of the mesh on it"));
        assert(deform != Deform::Morph
            || (deformer < mMorphs.size() && mMorphs[deformer].mVertexCount == positions.size()
                && "a morph moves exactly the vertices of the mesh on it"));

        if (positions.size() > sVertexBlock || indices.size() > sIndexBlock)
            throw Error("a mesh of " + std::to_string(positions.size()) + " vertices and "
                + std::to_string(indices.size()) + " indices is past the " + std::to_string(sVertexBlock) + " and "
                + std::to_string(sIndexBlock) + " one block of the shared buffers holds");

        ++mStructureRevision;
        ++mMeshRevision;

        const Span vertices = mVertexRuns.allocate(static_cast<Index>(positions.size()));
        const Span elements = mIndexRuns.allocate(static_cast<Index>(indices.size()));

        // Grown to what the allocators now reach, so the write below lands in room that exists, and
        // never shrunk: a run given back at the end goes to the allocator and the next mesh lands in
        // it rather than in a buffer that had to be resized twice. The attribute buffers stay
        // parallel to the position buffer whether or not the mesh brought the attribute, so a shader
        // can index all of them with one vertex id.
        //
        // **As long as the allocator reaches and no longer.** The blocks decide where a run may go,
        // not how much room is held: rounding this up to a whole block would leave the tail of the
        // last one uploaded to the device as well, which is megabytes of nothing per scene.
        if (mPositions.size() < mVertexRuns.getEnd())
        {
            mPositions.resize(mVertexRuns.getEnd());
            mNormals.resize(mPositions.size());
            mTexCoords.resize(mPositions.size());
        }

        if (mIndices.size() < mIndexRuns.getEnd())
            mIndices.resize(mIndexRuns.getEnd());

        MeshRange range{
            .mVertexOffset = vertices.mOffset,
            .mVertexCount = vertices.mCount,
            .mIndexOffset = elements.mOffset,
            .mIndexCount = elements.mCount,
            .mShape = shape,
            .mDeform = deform,
            .mDeformer = deformer,
            .mMaterial = material,
            .mBounds = boundsOf(positions),
        };

        // **A run in the bind table and a run of rows or weights, for a mesh that deforms.** The
        // rows are zeroed, which is a pose nothing can equal, and `mPosed` is what says the first
        // pose names the mesh regardless. Grown like the vertex buffers: to what the allocator
        // reaches, never shrunk.
        if (deform == Deform::Rig)
        {
            Rig& rig = mRigs[deformer];
            ++rig.mUses;
            range.mBindOffset = mBindRuns.allocate(vertices.mCount).mOffset;
            range.mPoseOffset = mBoneRuns.allocate(rig.mBoneCount).mOffset;
            if (mBones.size() < mBoneRuns.getEnd())
                mBones.resize(mBoneRuns.getEnd());
            std::fill_n(mBones.begin() + range.mPoseOffset, rig.mBoneCount, Shaders::GpuBone{});
        }
        else if (deform == Deform::Morph)
        {
            Morph& morph = mMorphs[deformer];
            ++morph.mUses;
            range.mBindOffset = mBindRuns.allocate(vertices.mCount).mOffset;
            range.mPoseOffset = mWeightRuns.allocate(morph.mTargetCount).mOffset;
            if (mWeights.size() < mWeightRuns.getEnd())
                mWeights.resize(mWeightRuns.getEnd());
            std::fill_n(mWeights.begin() + range.mPoseOffset, morph.mTargetCount, 0.0f);
        }

        writeMesh(range, positions, normals, texCoords, indices);

        const Index index = takeSlot(mMeshes, mFreeMeshes, range);
        noteMesh(index, SlotNews::Arrived);
        return index;
    }

    void SceneDesc::noteMesh(Index slot, SlotNews what)
    {
        // Grown here rather than beside every push, so the two stay parallel in one place. A resize
        // to the size it already is does not allocate, which is what the frame path pays.
        mMeshNews.resize(mMeshes.size(), SlotNews::None);
        mDeformedFlags.resize(mMeshes.size(), 0);
        note(slot, what, mMeshNews, mArrivedMeshes, mFreedMeshes);
    }

    void SceneDesc::noteTexture(Index slot, SlotNews what)
    {
        mTextureNews.resize(mTextures.size(), SlotNews::None);
        note(slot, what, mTextureNews, mArrivedTextures, mFreedTextures);
    }

    void SceneDesc::noteMaterial(Index slot)
    {
        mMaterialWritten.resize(mMaterials.size(), 0);
        assert(slot < mMaterialWritten.size());

        if (mMaterialWritten[slot] != 0)
            return;

        mMaterialWritten[slot] = 1;
        mWrittenMaterials.push_back(slot);
    }

    void SceneDesc::note(
        Index slot, SlotNews what, std::vector<SlotNews>& news, std::vector<Index>& arrived, std::vector<Index>& freed)
    {
        assert(what != SlotNews::None);
        assert(slot < news.size());

        SlotNews& standing = news[slot];
        if (standing == what)
            return;

        if (standing == SlotNews::Arrived)
            std::erase(arrived, slot);
        else if (standing == SlotNews::Freed)
            std::erase(freed, slot);

        standing = what;
        (what == SlotNews::Arrived ? arrived : freed).push_back(slot);
    }

    void SceneDesc::writeMesh(const MeshRange& range, std::span<const osg::Vec3f> positions,
        std::span<const osg::Vec3f> normals, std::span<const osg::Vec2f> texCoords,
        std::span<const std::uint32_t> indices)
    {
        std::copy(positions.begin(), positions.end(), mPositions.begin() + range.mVertexOffset);
        std::copy(indices.begin(), indices.end(), mIndices.begin() + range.mIndexOffset);

        // **Zeroed where the mesh brought none**, rather than left holding whatever the slot's last
        // tenant had. A reused slot is the only way that could happen and it would light a surface
        // by somebody else's normals.
        if (normals.empty())
            std::fill_n(mNormals.begin() + range.mVertexOffset, range.mVertexCount, osg::Vec3f());
        else
            std::copy(normals.begin(), normals.end(), mNormals.begin() + range.mVertexOffset);

        if (texCoords.empty())
            std::fill_n(mTexCoords.begin() + range.mVertexOffset, range.mVertexCount, osg::Vec2f());
        else
            std::copy(texCoords.begin(), texCoords.end(), mTexCoords.begin() + range.mVertexOffset);
    }

    Index SceneDesc::addRig(
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

    Index SceneDesc::addMorph(std::span<const osg::Vec3f> offsets, Index targets)
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

    void SceneDesc::poseRig(Index mesh, std::span<const Shaders::GpuBone> bones, const osg::BoundingBoxf& bounds)
    {
        assert(mesh < mMeshes.size());
        MeshRange& range = mMeshes[mesh];
        assert(range.mDeform == Deform::Rig && "a pose of rows for a mesh no rig skins");
        assert(bones.size() == mRigs[range.mDeformer].mBoneCount && "one row per bone of the rig, and no other count");

        if (takePose(bones, mBones.data() + range.mPoseOffset, range.mPosed))
            notePosed(mesh, bounds);
    }

    void SceneDesc::poseMorph(Index mesh, std::span<const float> weights, const osg::BoundingBoxf& bounds)
    {
        assert(mesh < mMeshes.size());
        MeshRange& range = mMeshes[mesh];
        assert(range.mDeform == Deform::Morph && "a pose of weights for a mesh no morph moves");
        assert(weights.size() == mMorphs[range.mDeformer].mTargetCount
            && "one weight per target of the morph, and no other count");

        if (takePose(weights, mWeights.data() + range.mPoseOffset, range.mPosed))
            notePosed(mesh, bounds);
    }

    void SceneDesc::notePosed(Index mesh, const osg::BoundingBoxf& bounds)
    {
        MeshRange& range = mMeshes[mesh];
        range.mPosed = true;

        // **A pose the size of the last one still reaches somewhere else.** An arm that came down is
        // the same count of vertices in a different place, and a box left where the bind pose put it
        // is what a camera would then be framed from.
        range.mBounds = bounds;

        // Named once however many callers reach it, because a backend builds one structure per mesh
        // and building it twice in a frame is the same answer for twice the cost.
        assert(mesh < mDeformedFlags.size());
        if (mDeformedFlags[mesh] == 0)
        {
            mDeformedFlags[mesh] = 1;
            mDeformed.push_back(mesh);
        }
    }

    std::span<const Shaders::GpuBone> SceneDesc::getMeshBones(Index mesh) const
    {
        assert(mesh < mMeshes.size());
        const MeshRange& range = mMeshes[mesh];
        assert(range.mDeform == Deform::Rig);
        return std::span(mBones).subspan(range.mPoseOffset, mRigs[range.mDeformer].mBoneCount);
    }

    std::span<const float> SceneDesc::getMeshWeights(Index mesh) const
    {
        assert(mesh < mMeshes.size());
        const MeshRange& range = mMeshes[mesh];
        assert(range.mDeform == Deform::Morph);
        return std::span(mWeights).subspan(range.mPoseOffset, mMorphs[range.mDeformer].mTargetCount);
    }

    void SceneDesc::releaseDeformer(MeshRange& range)
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

    Index SceneDesc::addMaterial(const Material& material)
    {
        holdMaterialTextures(material);

        const Index index = takeSlot(mMaterials, mFreeMaterials, material);
        noteMaterial(index);
        return index;
    }

    void SceneDesc::setMaterial(Index material, const Material& what)
    {
        assert(material < mMaterials.size());

        if (mMaterials[material] == what)
            return;

        const bool reclassed = mMaterials[material].getTraversed() != what.getTraversed();

        // **The new set taken before the old is given back.** A flipbook that comes round to a frame
        // it already had names the same texture twice running; releasing first would take that slot
        // to zero, empty its path and hand it to the next thing that asked — a slot changing
        // identity under everything standing on it, on a frame where nothing was supposed to move.
        holdMaterialTextures(what);
        dropMaterialTextures(mMaterials[material]);

        mMaterials[material] = what;
        noteMaterial(material);

        // Linear over the placements on the frame a surface crosses opaque, which a fade does twice
        // in its life; the flipbooks and the scrolls that animate every frame never come here.
        if (reclassed)
            for (Index slot = 0; slot < mInstances.size(); ++slot)
                if (mInstances[slot].isPlaced() && mInstances[slot].mMaterial == material)
                    mMoved.push_back(slot);
    }

    void SceneDesc::holdTexture(Index texture)
    {
        if (texture == sNoIndex)
            return;

        assert(texture < mTextureRefs.size());
        ++mTextureRefs[texture];
    }

    void SceneDesc::dropTexture(Index texture)
    {
        if (texture == sNoIndex)
            return;

        assert(texture < mTextureRefs.size());
        assert(mTextureRefs[texture] > 0 && "a texture given back more often than it was taken");

        if (--mTextureRefs[texture] > 0)
            return;

        // The name leaves the lookup with the slot, or the next reference to it resolves to a slot
        // nothing is standing in. Whichever of the two named it, and never both: a slot is a file or
        // it is something this renderer made.
        if (!mTextures[texture].empty())
        {
            mTextureIndex.erase(mTextures[texture]);
            mTextures[texture] = VFS::Path::Normalized();
        }
        else
        {
            assert(!mBaked[texture].empty() && "a slot with a reference to give back that nothing ever named");
            mBakedIndex.erase(mBaked[texture]);
            mBaked[texture].clear();
        }

        mFreeTextures.push_back(texture);
        noteTexture(texture, SlotNews::Freed);
    }

    void SceneDesc::holdMaterialTextures(const Material& material)
    {
        holdTexture(material.mDiffuse);
        holdTexture(material.mNormal);
        holdTexture(material.mEmissive);

        // The run is already in the layer table: a caller builds its layers, places them with
        // `addLayers` and then hands over a material naming where they landed.
        for (Index at = 0; at < material.mLayerCount; ++at)
            holdTexture(mLayers[material.mLayerOffset + at].mDiffuse);
    }

    void SceneDesc::dropMaterialTextures(const Material& material)
    {
        dropTexture(material.mDiffuse);
        dropTexture(material.mNormal);
        dropTexture(material.mEmissive);

        for (Index at = 0; at < material.mLayerCount; ++at)
            dropTexture(mLayers[material.mLayerOffset + at].mDiffuse);
    }

    Index SceneDesc::addMask(std::span<const float> weights)
    {
        assert(!weights.empty());

        const Span run = mMaskRuns.allocate(static_cast<std::uint32_t>(weights.size()));

        // Grown and never shrunk: a hole at the end gives its room back to the allocator, and the
        // next chunk to arrive lands in it rather than in a table that had to be resized twice.
        if (mMasks.size() < mMaskRuns.getEnd())
            mMasks.resize(mMaskRuns.getEnd());

        std::copy(weights.begin(), weights.end(), mMasks.begin() + run.mOffset);
        mArrivedMasks.push_back(run);
        return run.mOffset;
    }

    Span SceneDesc::addLayers(std::span<const MaterialLayer> layers)
    {
        assert(!layers.empty());

        const Span run = mLayerRuns.allocate(static_cast<std::uint32_t>(layers.size()));

        if (mLayers.size() < mLayerRuns.getEnd())
            mLayers.resize(mLayerRuns.getEnd());

        std::copy(layers.begin(), layers.end(), mLayers.begin() + run.mOffset);
        mArrivedLayers.push_back(run);
        return run;
    }

    void SceneDesc::addLight(const Light& light)
    {
        mLights.push_back(light);
    }

    void SceneDesc::addEmitter(std::span<const Sprite> sprites, Index texture, bool additive, const osg::Vec3f& across,
        const osg::Vec3f& upward, Index lighting)
    {

        if (sprites.empty())
            return;

        // The centre of the sprites' own bounding box rather than their mean, and the reach measured
        // off it: a plume is a handful of parcels strung along one axis, and a mean sits where most
        // of them happen to be at this instant rather than where the extent is.
        // **A quad's reach is its own diagonal, not its size.** An eye-facing sprite is a disc of
        // `mRadius`; a fixed one is a rectangle whose axes carry their own lengths, and a rain streak
        // ten times as tall as it is wide would be cut off by a sphere measured on the width.
        const float span = across.length2() > 0.0f || upward.length2() > 0.0f ? (across + upward).length() : 1.0f;

        osg::BoundingBoxf box;
        for (const Sprite& sprite : sprites)
        {
            const osg::Vec3f rim(sprite.mRadius * span, sprite.mRadius * span, sprite.mRadius * span);
            box.expandBy(sprite.mPosition - rim);
            box.expandBy(sprite.mPosition + rim);
        }

        const osg::Vec3f centre = box.center();
        float reach = 0.0f;
        for (const Sprite& sprite : sprites)
            reach = std::max(reach, (sprite.mPosition - centre).length() + sprite.mRadius * span);

        mEmitters.push_back(SpriteEmitter{
            .mCentre = centre,
            .mReach = reach,
            .mFirst = static_cast<Index>(mSprites.size()),
            .mCount = static_cast<Index>(sprites.size()),
            .mTexture = texture,
            .mLighting = lighting,
            .mAdditive = additive,
            .mAcross = across,

            .mUpward = upward,
        });

        mSprites.insert(mSprites.end(), sprites.begin(), sprites.end());
    }

    Index SceneDesc::takeTextureSlot()
    {
        ++mStructureRevision;

        // One size, so any freed slot will do — the array element it names is written over wherever
        // it sits, which is what the arrivals list is for.
        if (mFreeTextures.empty())
        {
            mTextures.emplace_back();
            mBaked.emplace_back();
            mTextureRefs.push_back(0);
            return static_cast<Index>(mTextures.size() - 1);
        }

        const Index index = mFreeTextures.back();
        mFreeTextures.pop_back();
        assert(mTextureRefs[index] == 0 && "a free slot something still names");

        return index;
    }

    Index SceneDesc::addTexture(VFS::Path::NormalizedView path)
    {
        const auto known = mTextureIndex.find(path);
        if (known != mTextureIndex.end())
            return known->second;

        const Index index = takeTextureSlot();
        mTextures[index] = path;

        mTextureIndex.emplace(path, index);
        noteTexture(index, SlotNews::Arrived);
        return index;
    }

    Index SceneDesc::addBakedTexture(std::string_view key)
    {
        assert(!key.empty() && "a baked texture with no key is one nothing can find again");

        const auto known = mBakedIndex.find(key);
        if (known != mBakedIndex.end())
            return known->second;

        const Index index = takeTextureSlot();
        mBaked[index] = key;

        mBakedIndex.emplace(key, index);
        noteTexture(index, SlotNews::Arrived);
        return index;
    }

    Index SceneDesc::addInstance(const MeshInstance& instance)
    {
        assert(instance.mMesh < mMeshes.size());
        assert(instance.mMaterial == sNoIndex || instance.mMaterial < mMaterials.size());

        Index slot;
        if (mFreeSlots.empty())
        {
            slot = static_cast<Index>(mInstances.size());
            mInstances.emplace_back();
            mPrevious.emplace_back();
        }
        else
        {
            slot = mFreeSlots.back();
            mFreeSlots.pop_back();
        }

        mInstances[slot] = instance;

        // **Standing where it is, not arriving from wherever the last tenant left.** A reused slot
        // would otherwise inherit a previous transform from something else entirely, and a motion
        // vector built from that points across the frame.
        mPrevious[slot] = instance.mTransform;

        mMoved.push_back(slot);
        ++mPlacedCount;
        return slot;
    }

    void SceneDesc::fadeInstance(Index slot, float opacity)
    {
        assert(slot < mInstances.size());
        assert(mInstances[slot].isPlaced() && "a slot nothing stands in");

        if (mInstances[slot].mOpacity == opacity)
            return;

        mInstances[slot].mOpacity = opacity;
        mMoved.push_back(slot);
    }

    bool SceneDesc::moveInstance(Index slot, const osg::Matrixf& transform)
    {
        assert(slot < mInstances.size());
        assert(mInstances[slot].isPlaced() && "a slot nothing stands in");

        if (mInstances[slot].mTransform == transform)
            return false;

        mInstances[slot].mTransform = transform;
        mMoved.push_back(slot);
        return true;
    }

    void SceneDesc::dropInstance(Index slot)
    {
        assert(slot < mInstances.size());
        assert(mInstances[slot].isPlaced() && "a slot dropped twice, or one nothing stood in");

        mInstances[slot] = MeshInstance{};
        mFreeSlots.push_back(slot);
        mMoved.push_back(slot);
        --mPlacedCount;
    }

    void SceneDesc::orderLights()
    {
        // **A total order and not a distance**, so that two lights the walk could hand over either
        // way round come out the same way round every time. Position separates all but the lamps
        // standing in one another, and what they carry separates those.
        std::sort(mLights.begin(), mLights.end(), [](const Light& a, const Light& b) {
            return std::make_tuple(a.mPosition.x(), a.mPosition.y(), a.mPosition.z(), a.mIntensity.x(),
                       a.mIntensity.y(), a.mIntensity.z(), a.mReach, a.mSourceRadius, a.mClearance)
                < std::make_tuple(b.mPosition.x(), b.mPosition.y(), b.mPosition.z(), b.mIntensity.x(), b.mIntensity.y(),
                    b.mIntensity.z(), b.mReach, b.mSourceRadius, b.mClearance);
        });
    }

    void SceneDesc::advancePlacement()
    {
        for (const Index slot : mMoved)
            mPrevious[slot] = mInstances[slot].mTransform;

        // Swapped and not copied: the two lists trade buffers, and neither allocates on the frame.
        mSettled.swap(mMoved);
        mMoved.clear();
    }

    void SceneDesc::clearPlacement()
    {
        mLights.clear();

        // The flags and the list say one thing between them, so they are emptied together — over
        // the frame's movers, and never over every mesh slot the scene holds.
        for (const Index mesh : mDeformed)
            mDeformedFlags[mesh] = 0;

        mDeformed.clear();
        mSprites.clear();
        mEmitters.clear();
    }

    namespace
    {
        /// A byte per entry, set for everything `keep` names. Duplicates and any order are fine.
        void markKept(std::vector<char>& flags, std::size_t count, std::span<const Index> keep)
        {
            // Cleared before it is grown, so the fill reaches every row rather than only the rows
            // past the length the last sweep left.
            flags.clear();
            flags.resize(count, 0);

            for (const Index index : keep)
            {
                assert(index < count);
                flags[index] = 1;
            }
        }
    }

    bool SceneDesc::release(std::span<const Index> meshes, std::span<const Index> materials)
    {
        // **The ordinary frame, and it costs two comparisons.** Both keep sets come from an identity
        // map keyed one-to-one on what produced the entry, so a set as large as the live table is
        // the whole of it — and a table with as many survivors as entries has nothing to free.
        //
        // Only meshes and materials are asked, and that is now the whole of what this frees: a
        // texture goes when the last material or hold naming it lets go, wherever that happens.
        assert(meshes.size() <= mMeshes.size());
        assert(materials.size() <= mMaterials.size());

        const std::size_t liveMeshes = mMeshes.size() - mFreeMeshes.size();
        const std::size_t liveMaterials = mMaterials.size() - mFreeMaterials.size();
        if (meshes.size() == liveMeshes && materials.size() == liveMaterials)
            return false;

        markKept(mKeptMeshes, mMeshes.size(), meshes);
        markKept(mKeptMaterials, mMaterials.size(), materials);

        // A slot already free is not one to free again.
        for (const Index slot : mFreeMeshes)
            mKeptMeshes[slot] = 1;

        for (const Index slot : mFreeMaterials)
            mKeptMaterials[slot] = 1;

        std::size_t freedMeshes = 0;
        for (Index index = 0; index < mMeshes.size(); ++index)
        {
            if (mKeptMeshes[index] != 0)
                continue;

            // **The slot stays where it is and only its geometry goes back.** Nothing is moved down
            // over it, so every index above this one still means what it meant — which is the whole
            // point, because each of them names a bottom-level acceleration structure that would
            // otherwise have to be built again. The room the geometry occupied returns to the
            // allocators, which merge it with whatever it touches: a cell arrived as thousands of
            // runs laid end to end and it leaves as the one hole it came as.
            MeshRange& range = mMeshes[index];
            mVertexRuns.release(Span{ .mOffset = range.mVertexOffset, .mCount = range.mVertexCount });
            mIndexRuns.release(Span{ .mOffset = range.mIndexOffset, .mCount = range.mIndexCount });
            releaseDeformer(range);

            range.mVertexCount = 0;
            range.mIndexCount = 0;
            range.mMaterial = sNoIndex;
            range.mBounds = osg::BoundingBoxf();

            // A slot given back names no structure to refit, however it was posed this frame: the
            // structure has gone with it. The list is compacted once below rather than searched
            // once per slot freed.
            mDeformedFlags[index] = 0;

            mFreeMeshes.push_back(index);
            noteMesh(index, SlotNews::Freed);
            ++freedMeshes;
        }

        // One pass over the frame's movers, on the frame a cell leaves, rather than one per slot
        // freed: a cell can give back thousands of slots and a crowd can be posing hundreds.
        if (freedMeshes > 0)
            std::erase_if(mDeformed, [this](const Index mesh) { return mDeformedFlags[mesh] == 0; });

        std::size_t freedMaterials = 0;
        for (Index index = 0; index < mMaterials.size(); ++index)
        {
            if (mKeptMaterials[index] != 0)
                continue;

            // **What it named goes with it**, and before its layer run does: the run is what says
            // which textures those were, and it is about to be handed to an allocator that will let
            // the next chunk write over it.
            const Material& going = mMaterials[index];
            dropMaterialTextures(going);

            // **Its layers and the masks behind them go with it.** A material that carries layers is
            // a terrain chunk, so without this what accumulates is a blend map per chunk walked
            // past; the runs are variable length, which is why they are given back to an allocator
            // rather than to a list of slots.
            for (Index at = 0; at < going.mLayerCount; ++at)
            {
                const MaterialLayer& layer = mLayers[going.mLayerOffset + at];
                mMaskRuns.release(Span{ .mOffset = layer.mMaskOffset,
                    .mCount = static_cast<std::uint32_t>(layer.mMaskWidth) * layer.mMaskHeight });
            }

            if (going.mLayerCount > 0)
                mLayerRuns.release(Span{ .mOffset = going.mLayerOffset, .mCount = going.mLayerCount });

            mMaterials[index] = Material{};
            mFreeMaterials.push_back(index);
            ++freedMaterials;
        }

        // **The per-frame lists are left as the walk left them.** Emptying them here read as "the
        // walk that comes next refills them", and that walk is the *next frame's* — one frame after
        // the picture this one is about to hand over, so a caller that uploads in between drew a
        // frame with no lights, sprites or emitters in it at all.
        //
        // Nothing in them can be stale either: a sweep is only sound straight after a walk of the
        // whole world (`SceneExtractor::retire`), so what is in them came from nodes that walk met —
        // the survivors, by the same marking this frees against.
        //
        // **Neither is a structure change, and neither is a shading change.** Nothing arrived and
        // nothing moved: the structures built from these indices are still correct, they simply
        // describe geometry nothing stands on any more, and the top level a frame rebuilds anyway is
        // what stops them being traced. A freed material's row, layers and masks are read by nothing
        // either, so no table has to be written for them — the next thing to land in the slot or the
        // run is what names it.
        return freedMeshes > 0 || freedMaterials > 0;
    }

    void SceneDesc::clear()
    {
        ++mStructureRevision;
        ++mMeshRevision;
        ++mResetRevision;
        mPositions.clear();
        mNormals.clear();
        mTexCoords.clear();
        mIndices.clear();
        mMeshes.clear();
        mDeformed.clear();
        mDeformedFlags.clear();
        mRigs.clear();
        mRuns.clear();
        mInfluences.clear();
        mMorphs.clear();
        mMorphOffsets.clear();
        mBones.clear();
        mWeights.clear();
        mFreeRigs.clear();
        mFreeMorphs.clear();
        mArrivedRigs.clear();
        mArrivedMorphs.clear();
        mBindRuns.clear();
        mBoneRuns.clear();
        mWeightRuns.clear();
        mRigRuns.clear();
        mInfluenceRuns.clear();
        mMorphRuns.clear();
        mInstances.clear();
        mPrevious.clear();
        mMoved.clear();
        mSettled.clear();
        mFreeSlots.clear();
        mPlacedCount = 0;
        mMaterials.clear();
        mLayers.clear();
        mMasks.clear();
        mLights.clear();
        mSprites.clear();
        mEmitters.clear();
        mTextures.clear();
        mBaked.clear();
        mTextureRefs.clear();
        mTextureIndex.clear();
        mBakedIndex.clear();
        mFreeMeshes.clear();
        mFreeMaterials.clear();
        mFreeTextures.clear();
        mVertexRuns.clear();
        mIndexRuns.clear();
        mLayerRuns.clear();
        mMaskRuns.clear();
        mArrivedTextures.clear();
        mArrivedMeshes.clear();
        mFreedTextures.clear();
        mFreedMeshes.clear();
        mTextureNews.clear();
        mMeshNews.clear();
        mWrittenMaterials.clear();
        mMaterialWritten.clear();
        mArrivedLayers.clear();
        mArrivedMasks.clear();
    }

    void SceneDesc::clearArrivals()
    {
        // Only the slots that have news are reset, rather than the whole of both tables: a
        // worldspace is thousands of meshes and what a frame changes is tens.
        for (const Index slot : mArrivedMeshes)
            mMeshNews[slot] = SlotNews::None;
        for (const Index slot : mFreedMeshes)
            mMeshNews[slot] = SlotNews::None;
        for (const Index slot : mArrivedTextures)
            mTextureNews[slot] = SlotNews::None;
        for (const Index slot : mFreedTextures)
            mTextureNews[slot] = SlotNews::None;
        for (const Index slot : mWrittenMaterials)
            mMaterialWritten[slot] = 0;

        mArrivedMeshes.clear();
        mFreedMeshes.clear();
        mArrivedTextures.clear();
        mFreedTextures.clear();
        mWrittenMaterials.clear();
        mArrivedLayers.clear();
        mArrivedMasks.clear();
        mArrivedRigs.clear();
        mArrivedMorphs.clear();
    }

    std::span<const osg::Vec3f> SceneDesc::getMeshPositions(Index mesh) const
    {
        assert(mesh < mMeshes.size());
        const MeshRange& range = mMeshes[mesh];
        return std::span(mPositions).subspan(range.mVertexOffset, range.mVertexCount);
    }

    std::span<const std::uint32_t> SceneDesc::getMeshIndices(Index mesh) const
    {
        assert(mesh < mMeshes.size());
        const MeshRange& range = mMeshes[mesh];
        return std::span(mIndices).subspan(range.mIndexOffset, range.mIndexCount);
    }

    std::uint32_t SceneDesc::getTriangleCount() const
    {
        return static_cast<std::uint32_t>(mIndices.size() / 3);
    }

    template <class Visit>
    void SceneDesc::forEachPlacement(Visit&& visit) const
    {
        for (const MeshInstance& instance : mInstances)
        {
            if (!instance.isPlaced())
                continue;

            // **Each mesh's own box carried through its instances**, rather than every vertex of
            // every instance — the difference between eight transforms per instance and several
            // hundred. The mesh kept it as its vertices arrived, so nothing is walked here at all.
            const osg::BoundingBoxf& box = mMeshes[instance.mMesh].mBounds;
            if (!box.valid())
                continue;

            osg::BoundingBoxf placed;
            for (unsigned int corner = 0; corner < 8; ++corner)
                placed.expandBy(box.corner(corner) * instance.mTransform);

            visit(instance, placed);
        }
    }

    osg::BoundingBoxf SceneDesc::getBounds() const
    {
        osg::BoundingBoxf bounds;
        forEachPlacement([&](const MeshInstance&, const osg::BoundingBoxf& box) { bounds.expandBy(box); });

        return bounds;
    }

    osg::BoundingBoxf SceneDesc::getContentBoundsWithin(const osg::BoundingBoxf& region) const
    {
        osg::BoundingBoxf bounds;
        forEachPlacement([&](const MeshInstance& instance, const osg::BoundingBoxf& box) {
            // An instance with no material is not a backdrop — the untextured test scenes place
            // those, and a caller framing one means to see it.
            if (instance.mMaterial != sNoIndex && mMaterials[instance.mMaterial].mKind == MaterialKind::Water)
                return;

            if (!box.intersects(region))
                return;

            // The part inside, so a chunk straddling the edge contributes where it overlaps rather
            // than dragging the answer out by its whole width.
            bounds.expandBy(osg::BoundingBoxf(std::max(box.xMin(), region.xMin()), std::max(box.yMin(), region.yMin()),
                std::max(box.zMin(), region.zMin()), std::min(box.xMax(), region.xMax()),
                std::min(box.yMax(), region.yMax()), std::min(box.zMax(), region.zMax())));
        });

        return bounds;
    }

    std::size_t SceneDesc::getGeometryBytes() const
    {
        return mPositions.size() * sizeof(osg::Vec3f) + mNormals.size() * sizeof(osg::Vec3f)
            + mTexCoords.size() * sizeof(osg::Vec2f) + mIndices.size() * sizeof(std::uint32_t);
    }
}
