#include "meshtable.hpp"

#include <algorithm>
#include <cassert>
#include <string>

#include "error.hpp"

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
                && mDeformers.getRigs()[deformer].getVertexCount() == positions.size()
                && "a rig skins exactly the vertices of the mesh on it"));
        assert(deform != Deform::Morph
            || (deformer < mDeformers.getMorphs().size()
                && mDeformers.getMorphs()[deformer].getVertexCount() == positions.size()
                && "a morph moves exactly the vertices of the mesh on it"));

        if (positions.size() > sVertexBlock || indices.size() > sIndexBlock)
            throw Error("a mesh of " + std::to_string(positions.size()) + " vertices and "
                + std::to_string(indices.size()) + " indices is past the " + std::to_string(sVertexBlock) + " and "
                + std::to_string(sIndexBlock) + " one block of the shared buffers holds");

        ++mRevision;

        const Run vertices = mPositions.allocate(positions);
        const Run elements = mIndices.allocate(indices);

        MeshRange range{
            .mVertices = vertices,
            .mIndices = elements,
            .mShape = shape,
            .mDeform = deform,
            .mDeformer = deformer,
            .mMaterial = material,
            .mBounds = boundsOf(positions),
        };

        mDeformers.stand(range);

        writeAttributes(range, normals, texCoords);

        const Index index = mRows.take(range);
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

    void MeshTable::writeAttributes(
        const MeshRange& range, std::span<const osg::Vec3f> normals, std::span<const osg::Vec2f> texCoords)
    {
        // **As long as the positions and no longer.** All three arrays are indexed by one vertex
        // id, and the blocks decide where a run may go rather than how much room is held — so
        // rounding up to a whole block would upload the tail of the last one as well.
        const std::size_t reach = getPositions().size();
        mNormals.resize(reach);
        mTexCoords.resize(reach);

        // **Zeroed where the mesh brought none**, rather than left holding whatever the slot's last
        // tenant had. A reused slot is the only way that could happen and it would light a surface
        // by somebody else's normals.
        if (normals.empty())
            std::fill_n(mNormals.begin() + range.mVertices.mOffset, range.mVertices.mCount, osg::Vec3f());
        else
            std::copy(normals.begin(), normals.end(), mNormals.begin() + range.mVertices.mOffset);

        if (texCoords.empty())
            std::fill_n(mTexCoords.begin() + range.mVertices.mOffset, range.mVertices.mCount, osg::Vec2f());
        else
            std::copy(texCoords.begin(), texCoords.end(), mTexCoords.begin() + range.mVertices.mOffset);
    }

    void MeshTable::notePosed(Index mesh, const osg::BoundingBoxf& bounds)
    {
        MeshRange& range = mRows.at(mesh);
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
        const MeshRange& range = mRows.at(mesh);
        return range.mVertices.in(getPositions());
    }

    std::span<const std::uint32_t> MeshTable::getMeshIndices(Index mesh) const
    {
        const MeshRange& range = mRows.at(mesh);
        return range.mIndices.in(getIndices());
    }

    std::uint32_t MeshTable::getTriangleCount() const
    {
        return static_cast<std::uint32_t>(getIndices().size() / 3);
    }

    std::size_t MeshTable::mark(std::span<const Index> keep)
    {
        return mRows.mark(keep);
    }

    std::size_t MeshTable::sweep()
    {
        const std::size_t freed = mRows.sweep([this](const Index index, MeshRange& range) {
            // **The slot stays where it is and only its geometry goes back.** Nothing is moved down
            // over it, so every index above this one still means what it meant — which is the whole
            // point, because each of them names a bottom-level acceleration structure that would
            // otherwise have to be built again. The room the geometry occupied returns to the
            // allocators, which merge it with whatever it touches: a cell arrived as thousands of
            // runs laid end to end and it leaves as the one hole it came as.
            mPositions.release(range.mVertices);
            mIndices.release(range.mIndices);
            mDeformers.release(range);

            range.mVertices.mCount = 0;
            range.mIndices.mCount = 0;
            range.mMaterial = sNoIndex;
            range.mBounds = osg::BoundingBoxf();

            // A slot given back names no structure to refit, however it was posed this frame: the
            // structure has gone with it.
            mDeformed.remove(index);

            note(index, SlotNews::Freed);
        });

        // Both sets held a removal per row freed above, and each settles in one pass rather than
        // one per row.
        mDeformed.compact();
        mDeformers.compact();

        return freed;
    }

    std::size_t MeshTable::getGeometryBytes() const
    {
        return getPositions().size() * sizeof(osg::Vec3f) + mNormals.size() * sizeof(osg::Vec3f)
            + mTexCoords.size() * sizeof(osg::Vec2f) + getIndices().size() * sizeof(std::uint32_t);
    }

    void MeshTable::clearArrivals()
    {
        mChanges.clearArrivals();
    }
}
