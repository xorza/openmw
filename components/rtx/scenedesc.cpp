#include "scenedesc.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <tuple>

#include "error.hpp"
#include "slotrows.hpp"

namespace Rtx
{
    namespace
    {
        /// The box every one of `positions` fits in.
        osg::BoundingBoxf boundsOf(std::span<const osg::Vec3f> positions)
        {
            osg::BoundingBoxf bounds;
            for (const osg::Vec3f& position : positions)
                bounds.expandBy(position);

            return bounds;
        }
    }

    Index SceneDesc::addMesh(std::span<const osg::Vec3f> positions, std::span<const osg::Vec3f> normals,
        std::span<const osg::Vec2f> texCoords, std::span<const std::uint32_t> indices, FoldedShape shape, Deform deform,
        Index deformer, Index material)
    {
        assert(!positions.empty());
        assert(
            (material == sNoIndex || material < mMaterialTable.size()) && "a mesh wearing a material the scene lacks");
        assert(normals.empty() || normals.size() == positions.size());
        assert(texCoords.empty() || texCoords.size() == positions.size());
        assert(indices.size() % 3 == 0);
        assert(std::all_of(indices.begin(), indices.end(), [&](std::uint32_t i) { return i < positions.size(); }));
        assert((deform == Deform::None) == (deformer == sNoIndex) && "a deforming mesh names what poses it");
        assert(deform != Deform::Rig
            || (deformer < mDeformers.getRigs().size()
                && mDeformers.getRigs()[deformer].mVertexCount == positions.size()
                && "a rig skins exactly the vertices of the mesh on it"));
        assert(deform != Deform::Morph
            || (deformer < mDeformers.getMorphs().size()
                && mDeformers.getMorphs()[deformer].mVertexCount == positions.size()
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

        mDeformers.stand(range);

        writeMesh(range, positions, normals, texCoords, indices);

        const Index index = takeSlot(mMeshes, mFreeMeshes, range);
        noteMesh(index, SlotNews::Arrived);
        return index;
    }

    void SceneDesc::noteMesh(Index slot, SlotNews what)
    {
        // Grown here rather than beside every push, so everything keyed on a mesh slot reaches the
        // table's size in one place. A resize to the size it already is does not allocate, which is
        // what the frame path pays.
        mMeshChanges.grow(mMeshes.size());
        mDeformed.grow(mMeshes.size());
        mMeshChanges.note(slot, what);
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
        mDeformed.add(mesh);
    }

    Index SceneDesc::addRig(
        std::span<const std::uint32_t> runs, std::span<const Shaders::GpuInfluence> influences, Index boneCount)
    {
        return mDeformers.addRig(runs, influences, boneCount);
    }

    Index SceneDesc::addMorph(std::span<const osg::Vec3f> offsets, Index targets)
    {
        return mDeformers.addMorph(offsets, targets);
    }

    void SceneDesc::poseRig(Index mesh, std::span<const Shaders::GpuBone> bones, const osg::BoundingBoxf& bounds)
    {
        assert(mesh < mMeshes.size());
        if (mDeformers.poseRig(mMeshes[mesh], bones))
            notePosed(mesh, bounds);
    }

    void SceneDesc::poseMorph(Index mesh, std::span<const float> weights, const osg::BoundingBoxf& bounds)
    {
        assert(mesh < mMeshes.size());
        if (mDeformers.poseMorph(mMeshes[mesh], weights))
            notePosed(mesh, bounds);
    }

    std::span<const Shaders::GpuBone> SceneDesc::getMeshBones(Index mesh) const
    {
        assert(mesh < mMeshes.size());
        return mDeformers.getMeshBones(mMeshes[mesh]);
    }

    std::span<const float> SceneDesc::getMeshWeights(Index mesh) const
    {
        assert(mesh < mMeshes.size());
        return mDeformers.getMeshWeights(mMeshes[mesh]);
    }

    Index SceneDesc::addMaterial(const Material& material)
    {
        return mMaterialTable.add(material);
    }

    void SceneDesc::setMaterial(Index material, const Material& what)
    {
        if (!mMaterialTable.set(material, what))
            return;

        // Linear over the placements on the frame a surface crosses opaque, which a fade does twice
        // in its life; the flipbooks and the scrolls that animate every frame never come here.
        //
        // **Here and not in the table, because the placements are not the table's.** What a
        // material changed about traversal is the table's answer; which rows carry it is this.
        const std::span<const MeshInstance> placed = mPlacements.getAll();
        for (Index slot = 0; slot < placed.size(); ++slot)
            if (placed[slot].isPlaced() && placed[slot].mMaterial == material)
                mPlacements.rewrite(slot);
    }

    void SceneDesc::holdTexture(Index texture)
    {
        mTextures.hold(texture);
    }

    void SceneDesc::dropTexture(Index texture)
    {
        mTextures.drop(texture);
    }

    Index SceneDesc::addMask(std::span<const float> weights)
    {
        return mMaterialTable.addMask(weights);
    }

    Span SceneDesc::addLayers(std::span<const MaterialLayer> layers)
    {
        return mMaterialTable.addLayers(layers);
    }

    void SceneDesc::addLight(const Light& light)
    {
        mLights.push_back(light);
    }

    void SceneDesc::addEmitter(
        std::span<const Sprite> sprites, Index texture, bool additive, float width, Index lighting)
    {
        if (sprites.empty())
            return;

        // **A quad's reach is its own diagonal, not its size.** An eye-facing sprite is a disc of
        // `mRadius`; an oriented one is a rectangle as long as its axis and as wide as `width`, and
        // a rain streak ten times as tall as it is wide would be cut off by a sphere measured on the
        // width. The two are perpendicular wherever the march looks at the quad, because the width
        // is swung about the axis — so the corner is the diagonal of the two lengths.
        const auto spanOf = [width](const Sprite& sprite) {
            assert((width > 0.0f) == (sprite.mAxis.length2() > 0.0f)
                && "an emitter and its sprites disagree about whether the quads hang in the world");

            return width > 0.0f ? std::sqrt(sprite.mAxis.length2() + width * width) : 1.0f;
        };

        // The centre of the sprites' own bounding box rather than their mean, and the reach measured
        // off it: a plume is a handful of parcels strung along one axis, and a mean sits where most
        // of them happen to be at this instant rather than where the extent is.
        osg::BoundingBoxf box;
        for (const Sprite& sprite : sprites)
        {
            const float rim = sprite.mRadius * spanOf(sprite);
            box.expandBy(sprite.mPosition - osg::Vec3f(rim, rim, rim));
            box.expandBy(sprite.mPosition + osg::Vec3f(rim, rim, rim));
        }

        const osg::Vec3f centre = box.center();
        float reach = 0.0f;
        for (const Sprite& sprite : sprites)
            reach = std::max(reach, (sprite.mPosition - centre).length() + sprite.mRadius * spanOf(sprite));

        mEmitters.push_back(SpriteEmitter{
            .mCentre = centre,
            .mReach = reach,
            .mFirst = static_cast<Index>(mSprites.size()),
            .mCount = static_cast<Index>(sprites.size()),
            .mTexture = texture,
            .mLighting = lighting,
            .mAdditive = additive,
            .mWidth = width,
        });

        mSprites.insert(mSprites.end(), sprites.begin(), sprites.end());
    }

    Index SceneDesc::addTexture(VFS::Path::NormalizedView path)
    {
        return mTextures.add(path);
    }

    Index SceneDesc::addBakedTexture(std::string_view key)
    {
        return mTextures.addBaked(key);
    }

    Index SceneDesc::addInstance(const MeshInstance& instance)
    {
        assert(instance.mMesh < mMeshes.size());
        assert(instance.mMaterial == sNoIndex || instance.mMaterial < mMaterialTable.size());

        return mPlacements.add(instance);
    }

    void SceneDesc::fadeInstance(Index slot, float opacity)
    {
        mPlacements.fade(slot, opacity);
    }

    bool SceneDesc::moveInstance(Index slot, const osg::Matrixf& transform)
    {
        return mPlacements.move(slot, transform);
    }

    void SceneDesc::dropInstance(Index slot)
    {
        mPlacements.drop(slot);
    }

    void SceneDesc::orderLights()
    {
        // **A total order and not a distance**, so that two lights the walk could hand over either
        // way round come out the same way round every time. Position separates all but the lamps
        // standing in one another, and what they carry separates those.
        //
        // **Tied and not built**, because a sort of a cell's three hundred lamps compares thousands
        // of times and a tuple of references copies none of them. `osg::Vec3f` orders itself
        // lexicographically on x, y and z, which is what makes the two vectors here the same order
        // as their six components spelled out.
        std::sort(mLights.begin(), mLights.end(), [](const Light& a, const Light& b) {
            return std::tie(a.mPosition, a.mIntensity, a.mReach, a.mSourceRadius, a.mClearance)
                < std::tie(b.mPosition, b.mIntensity, b.mReach, b.mSourceRadius, b.mClearance);
        });
    }

    void SceneDesc::advancePlacement()
    {
        mPlacements.advance();
    }

    void SceneDesc::clearPlacement()
    {
        mLights.clear();

        mDeformed.clear();
        mSprites.clear();
        mEmitters.clear();
    }

    bool SceneDesc::release(std::span<const Index> meshes, std::span<const Index> materials)
    {
        // Only meshes and materials are asked, and that is now the whole of what this frees: a
        // texture goes when the last material or hold naming it lets go, wherever that happens.
        const std::size_t keptMeshes = markKept(mKeptMeshes, mMeshes.size(), meshes, mFreeMeshes);
        const std::size_t keptMaterials = mMaterialTable.mark(materials);

        // **The ordinary frame leaves here**: a table with as many survivors as live entries has
        // nothing to free, and what it paid for the answer is the marking above.
        //
        // **Asked of the marks and not of the span's length.** Those two agree only while the keep
        // set names each survivor once, which is a property of the identity map that fills it rather
        // than of this call — so a second way of collecting survivors cannot get it wrong.
        const std::size_t liveMeshes = mMeshes.size() - mFreeMeshes.size();
        if (keptMeshes == liveMeshes && keptMaterials == mMaterialTable.getLiveCount())
            return false;

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
            mDeformers.release(range);

            range.mVertexCount = 0;
            range.mIndexCount = 0;
            range.mMaterial = sNoIndex;
            range.mBounds = osg::BoundingBoxf();

            // A slot given back names no structure to refit, however it was posed this frame: the
            // structure has gone with it.
            mDeformed.remove(index);

            mFreeMeshes.push_back(index);
            noteMesh(index, SlotNews::Freed);
            ++freedMeshes;
        }

        mDeformed.compact();

        const std::size_t freedMaterials = mMaterialTable.sweep();

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

    void SceneDesc::clearArrivals()
    {
        mMeshChanges.clearArrivals();
        mTextures.clearArrivals();
        mMaterialTable.clearArrivals();
        mDeformers.clearArrivals();
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
        for (const MeshInstance& instance : mPlacements.getAll())
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
            if (instance.mMaterial != sNoIndex
                && mMaterialTable.getRows()[instance.mMaterial].mKind == MaterialKind::Water)
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
