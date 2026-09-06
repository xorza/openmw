#include "meshtable.hpp"

#include <algorithm>
#include <cassert>
#include <string>

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

    Index MeshTable::add(std::span<const osg::Vec3f> positions, std::span<const osg::Vec3f> normals,
        std::span<const osg::Vec2f> texCoords, std::span<const std::uint32_t> indices, FoldedShape shape, Deform deform,
        Index deformer, Index material)
    {
        assert(!positions.empty());
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

        ++mRevision;

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

        write(range, positions, normals, texCoords, indices);

        const Index index = takeSlot(mRows, mFree, range);
        note(index, SlotNews::Arrived);
        return index;
    }

    void MeshTable::note(Index slot, SlotNews what)
    {
        // Grown here rather than beside every push, so everything keyed on a mesh slot reaches the
        // table's size in one place. A resize to the size it already is does not allocate, which is
        // what the frame path pays.
        mChanges.grow(mRows.size());
        mDeformed.grow(mRows.size());
        mChanges.note(slot, what);
    }

    void MeshTable::write(const MeshRange& range, std::span<const osg::Vec3f> positions,
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

    void MeshTable::notePosed(Index mesh, const osg::BoundingBoxf& bounds)
    {
        MeshRange& range = mRows[mesh];
        range.mPosed = true;

        // **A pose the size of the last one still reaches somewhere else.** An arm that came down is
        // the same count of vertices in a different place, and a box left where the bind pose put it
        // is what a camera would then be framed from.
        range.mBounds = bounds;

        // Named once however many callers reach it, because a backend builds one structure per mesh
        // and building it twice in a frame is the same answer for twice the cost.
        mDeformed.add(mesh);
    }

    std::span<const osg::Vec3f> MeshTable::getMeshPositions(Index mesh) const
    {
        assert(mesh < mRows.size());
        const MeshRange& range = mRows[mesh];
        return std::span(mPositions).subspan(range.mVertexOffset, range.mVertexCount);
    }

    std::span<const std::uint32_t> MeshTable::getMeshIndices(Index mesh) const
    {
        assert(mesh < mRows.size());
        const MeshRange& range = mRows[mesh];
        return std::span(mIndices).subspan(range.mIndexOffset, range.mIndexCount);
    }

    std::uint32_t MeshTable::getTriangleCount() const
    {
        return static_cast<std::uint32_t>(mIndices.size() / 3);
    }

    std::size_t MeshTable::mark(std::span<const Index> keep)
    {
        return markKept(mKept, mRows.size(), keep, mFree);
    }

    std::size_t MeshTable::sweep()
    {
        std::size_t freed = 0;
        for (Index index = 0; index < mRows.size(); ++index)
        {
            if (mKept[index] != 0)
                continue;

            // **The slot stays where it is and only its geometry goes back.** Nothing is moved down
            // over it, so every index above this one still means what it meant — which is the whole
            // point, because each of them names a bottom-level acceleration structure that would
            // otherwise have to be built again. The room the geometry occupied returns to the
            // allocators, which merge it with whatever it touches: a cell arrived as thousands of
            // runs laid end to end and it leaves as the one hole it came as.
            MeshRange& range = mRows[index];
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

            mFree.push_back(index);
            note(index, SlotNews::Freed);
            ++freed;
        }

        mDeformed.compact();
        return freed;
    }

    std::size_t MeshTable::getGeometryBytes() const
    {
        return mPositions.size() * sizeof(osg::Vec3f) + mNormals.size() * sizeof(osg::Vec3f)
            + mTexCoords.size() * sizeof(osg::Vec2f) + mIndices.size() * sizeof(std::uint32_t);
    }

    void MeshTable::clearArrivals()
    {
        mChanges.clearArrivals();
    }
}
