#include "scenedesc.hpp"

#include <algorithm>
#include <cassert>

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
        std::span<const osg::Vec2f> texCoords, std::span<const std::uint32_t> indices, bool sheet)
    {
        assert(!positions.empty());
        assert(normals.empty() || normals.size() == positions.size());
        assert(texCoords.empty() || texCoords.size() == positions.size());
        assert(indices.size() % 3 == 0);
        assert(std::all_of(indices.begin(), indices.end(), [&](std::uint32_t i) { return i < positions.size(); }));

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

        const MeshRange range{
            .mVertexOffset = vertices.mOffset,
            .mVertexCount = vertices.mCount,
            .mIndexOffset = elements.mOffset,
            .mIndexCount = elements.mCount,
            .mSheet = sheet,
        };

        writeMesh(range, positions, normals, texCoords, indices);

        // **Any free slot will do.** What had to fit is the geometry, and the allocators have already
        // placed it; a slot is one row of a table and every row is the same size.
        if (!mFreeMeshes.empty())
        {
            const Index index = mFreeMeshes.back();
            mFreeMeshes.pop_back();
            mMeshes[index] = range;
            noteMesh(index, SlotNews::Arrived);
            return index;
        }

        mMeshes.push_back(range);

        const Index index = static_cast<Index>(mMeshes.size() - 1);
        noteMesh(index, SlotNews::Arrived);
        return index;
    }

    void SceneDesc::noteMesh(Index slot, SlotNews what)
    {
        // Grown here rather than beside every push, so the two stay parallel in one place. A resize
        // to the size it already is does not allocate, which is what the frame path pays.
        mMeshNews.resize(mMeshes.size(), SlotNews::None);
        note(slot, what, mMeshNews, mArrivedMeshes, mFreedMeshes);
    }

    void SceneDesc::noteTexture(Index slot, SlotNews what)
    {
        mTextureNews.resize(mTextures.size(), SlotNews::None);
        note(slot, what, mTextureNews, mArrivedTextures, mFreedTextures);
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

    void SceneDesc::updateMesh(Index mesh, std::span<const osg::Vec3f> positions, std::span<const osg::Vec3f> normals)
    {
        assert(mesh < mMeshes.size());

        const MeshRange& range = mMeshes[mesh];
        assert(positions.size() == range.mVertexCount);
        assert(normals.empty() || normals.size() == range.mVertexCount);

        std::copy(positions.begin(), positions.end(), mPositions.begin() + range.mVertexOffset);
        if (!normals.empty())
            std::copy(normals.begin(), normals.end(), mNormals.begin() + range.mVertexOffset);

        // Named once however many callers reach it, because a backend builds one structure per mesh
        // and building it twice in a frame is the same answer for twice the cost. Linear over a list
        // that is the frame's moving meshes — a crowd, not a cell.
        if (std::find(mDeformed.begin(), mDeformed.end(), mesh) == mDeformed.end())
            mDeformed.push_back(mesh);
    }

    Index SceneDesc::addMaterial(const Material& material)
    {
        ++mShadingRevision;
        holdMaterialTextures(material);

        // One size, so any freed slot will do and the back of the list is as good as any of them.
        if (!mFreeMaterials.empty())
        {
            const Index index = mFreeMaterials.back();
            mFreeMaterials.pop_back();
            mMaterials[index] = material;
            return index;
        }

        mMaterials.push_back(material);
        return static_cast<Index>(mMaterials.size() - 1);
    }

    void SceneDesc::setMaterial(Index material, const Material& what)
    {
        assert(material < mMaterials.size());

        if (mMaterials[material] == what)
            return;

        // **The new set taken before the old is given back.** A flipbook that comes round to a frame
        // it already had names the same texture twice running; releasing first would take that slot
        // to zero, empty its path and hand it to the next thing that asked — a slot changing
        // identity under everything standing on it, on a frame where nothing was supposed to move.
        holdMaterialTextures(what);
        dropMaterialTextures(mMaterials[material]);

        mMaterials[material] = what;
        ++mShadingRevision;
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

        ++mShadingRevision;

        const Span run = mMaskRuns.allocate(static_cast<std::uint32_t>(weights.size()));

        // Grown and never shrunk: a hole at the end gives its room back to the allocator, and the
        // next chunk to arrive lands in it rather than in a table that had to be resized twice.
        if (mMasks.size() < mMaskRuns.getEnd())
            mMasks.resize(mMaskRuns.getEnd());

        std::copy(weights.begin(), weights.end(), mMasks.begin() + run.mOffset);
        return run.mOffset;
    }

    Span SceneDesc::addLayers(std::span<const MaterialLayer> layers)
    {
        assert(!layers.empty());

        ++mShadingRevision;

        const Span run = mLayerRuns.allocate(static_cast<std::uint32_t>(layers.size()));

        if (mLayers.size() < mLayerRuns.getEnd())
            mLayers.resize(mLayerRuns.getEnd());

        std::copy(layers.begin(), layers.end(), mLayers.begin() + run.mOffset);
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

        mInstances[slot].mOpacity = opacity;
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
        --mPlacedCount;
    }

    void SceneDesc::advancePlacement()
    {
        for (const Index slot : mMoved)
            mPrevious[slot] = mInstances[slot].mTransform;

        mMoved.clear();
    }

    void SceneDesc::clearPlacement()
    {
        mLights.clear();
        mDeformed.clear();
        mSprites.clear();
        mEmitters.clear();
    }

    namespace
    {
        /// A byte per entry, set for everything `keep` names. Duplicates and any order are fine.
        std::vector<char> keepFlags(std::size_t count, std::span<const Index> keep)
        {
            std::vector<char> flags(count, 0);
            for (const Index index : keep)
            {
                assert(index < count);
                flags[index] = 1;
            }

            return flags;
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

        // Allocated here rather than kept, because this runs on the frame a cell left and on no
        // other. What it is not allowed to do is allocate on the frames in between, which is what
        // the test above is for.
        std::vector<char> keptMesh = keepFlags(mMeshes.size(), meshes);
        std::vector<char> keptMaterial = keepFlags(mMaterials.size(), materials);

        // A slot already free is not one to free again.
        for (const Index slot : mFreeMeshes)
            keptMesh[slot] = 1;

        for (const Index slot : mFreeMaterials)
            keptMaterial[slot] = 1;

        std::size_t freedMeshes = 0;
        for (Index index = 0; index < mMeshes.size(); ++index)
        {
            if (keptMesh[index] != 0)
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

            range.mVertexCount = 0;
            range.mIndexCount = 0;

            mFreeMeshes.push_back(index);
            noteMesh(index, SlotNews::Freed);
            ++freedMeshes;
        }

        std::size_t freedMaterials = 0;
        for (Index index = 0; index < mMaterials.size(); ++index)
        {
            if (keptMaterial[index] != 0)
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
        if (freedMeshes > 0)
        {
            // **Not a structure change.** Nothing arrived and nothing moved: the structures built
            // from these indices are still correct, they simply describe geometry nothing stands on
            // any more, and the top level a frame rebuilds anyway is what stops them being traced.
            // Saying otherwise here is what made a cell boundary cost a full rebuild.
            ++mShadingRevision;
        }

        if (freedMaterials > 0)
            ++mShadingRevision;

        // **A texture going is not a shading change**, wherever it went. What the shading revision
        // drives is the upload of the materials, the layers and the masks, and none of them moved:
        // the slot keeps its index, nothing that names it survived, and the array holds its image
        // until something takes the slot over. Saying otherwise re-uploads those tables on every
        // crossing.
        return freedMeshes > 0 || freedMaterials > 0;
    }

    void SceneDesc::clear()
    {
        ++mStructureRevision;
        ++mShadingRevision;
        ++mMeshRevision;
        ++mResetRevision;
        mPositions.clear();
        mNormals.clear();
        mTexCoords.clear();
        mIndices.clear();
        mMeshes.clear();
        mDeformed.clear();
        mInstances.clear();
        mPrevious.clear();
        mMoved.clear();
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

        mArrivedMeshes.clear();
        mFreedMeshes.clear();
        mArrivedTextures.clear();
        mFreedTextures.clear();
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
        // **Each mesh's local box carried through its instances**, rather than every vertex of every
        // instance — the difference between eight transforms per instance and several hundred.
        std::vector<osg::BoundingBoxf> local(mMeshes.size());
        for (std::size_t i = 0; i < mMeshes.size(); ++i)
            for (const osg::Vec3f& position : getMeshPositions(static_cast<Index>(i)))
                local[i].expandBy(position);

        for (const MeshInstance& instance : mInstances)
        {
            if (!instance.isPlaced())
                continue;

            const osg::BoundingBoxf& box = local[instance.mMesh];
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
