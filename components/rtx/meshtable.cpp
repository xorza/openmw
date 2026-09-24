#include "meshtable.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

#include "tangent.hpp"

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

        /// A run of `runs` holding `brought`, with `values` reaching it.
        template <class T>
        Run put(RunAllocator& runs, BlockedValues<T>& values, std::span<const T> brought)
        {
            assert(!brought.empty() && "a run of nothing is not a run");

            const Run run = runs.allocate(static_cast<std::uint32_t>(brought.size()));
            values.reach(runs.getEnd());
            std::copy(brought.begin(), brought.end(), values.in(run).begin());

            return run;
        }
    }

    Result<void, std::string> MeshTable::checkFits(const MeshArrays& arrays)
    {
        const std::span<const osg::Vec3f> positions = arrays.mPositions;
        const std::span<const std::uint32_t> indices = arrays.mIndices;

        if (positions.size() > sVertexBlock || indices.size() > sIndexBlock)
            return Err{ "its " + std::to_string(positions.size()) + " vertices and " + std::to_string(indices.size())
                + " indices are past the " + std::to_string(sVertexBlock) + " and " + std::to_string(sIndexBlock)
                + " one block of the shared buffers holds" };

        return {};
    }

    Index MeshTable::add(
        DeformerTable& deformers, const MeshArrays& arrays, FoldedShape shape, Deform deform, Index deformer)
    {
        assert(checkFits(arrays).isOk() && "a mesh past a block, which its reader was to refuse");

        const std::span<const osg::Vec3f> positions = arrays.mPositions;
        const std::span<const std::uint32_t> indices = arrays.mIndices;

        assert(!positions.empty());
        assert(arrays.mNormals.empty() || arrays.mNormals.size() == positions.size());
        assert(arrays.mTexCoords.empty() || arrays.mTexCoords.size() == positions.size());
        assert(arrays.mSecondTexCoords.empty() || arrays.mSecondTexCoords.size() == positions.size());
        assert((arrays.mUnitStreams == 0 || !arrays.mSecondTexCoords.empty())
            && "a unit reads a second set the mesh did not bring");
        assert(arrays.mColours.empty() || arrays.mColours.size() == positions.size());
        assert(arrays.mTangents.empty() || arrays.mTangents.size() == positions.size());
        assert(indices.size() % 3 == 0);
        assert(std::all_of(indices.begin(), indices.end(), [&](std::uint32_t i) { return i < positions.size(); }));
        assert((deform == Deform::None) == (deformer == sNoIndex) && "a deforming mesh names what poses it");
        assert(deform == Deform::None
            || (deformer < deformers.getDeformers().size() && deformers.getDeformers()[deformer].mKind == deform
                && deformers.getDeformers()[deformer].getVertexCount() == positions.size()
                && "a deformer moves exactly the vertices of the mesh on it"));

        ++mRevision;

        const Run vertices = mVertexRuns.allocate(static_cast<std::uint32_t>(positions.size()));
        const Run elements = put(mIndexRuns, mIndices, indices);
        const Run second
            = arrays.mSecondTexCoords.empty() ? Run{} : put(mSecondRuns, mSecondTexCoords, arrays.mSecondTexCoords);

        MeshRange range{
            .mVertices = vertices,
            .mIndices = elements,
            .mSecondTexCoords = second,
            .mUnitStreams = arrays.mSecondTexCoords.empty() ? 0u : arrays.mUnitStreams,
            .mShape = shape,
            .mDeform = deform,
            .mDeformer = deformer,
            .mBounds = boundsOf(positions),
        };

        deformers.stand(range);

        writeVertices(range, arrays);

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

    void MeshTable::writeVertices(const MeshRange& range, const MeshArrays& arrays)
    {
        // As far as the runs reach and no further. All five are indexed by one vertex id, and the
        // blocks decide where a run may go rather than how much is uploaded — so reaching a whole
        // block would upload the tail of the last one as well.
        const std::uint32_t reach = mVertexRuns.getEnd();
        mPositions.reach(reach);
        mNormals.reach(reach);
        mTexCoords.reach(reach);
        mColours.reach(reach);
        mTangents.reach(reach);

        // Filled where the mesh brought none, or a reused slot lights a surface by its last
        // tenant's normals. A zero normal says "use the triangle's plane" and a white colour says
        // "no tint", because one is read and the other is multiplied.
        const auto fill = [&](auto& into, const auto& brought, const auto& nothing) {
            const auto at = into.in(range.mVertices).begin();
            if (brought.empty())
                std::fill_n(at, range.mVertices.mCount, nothing);
            else
                std::copy(brought.begin(), brought.end(), at);
        };

        fill(mPositions, arrays.mPositions, osg::Vec3f());
        fill(mNormals, arrays.mNormals, osg::Vec3f());
        fill(mTexCoords, arrays.mTexCoords, osg::Vec2f());
        fill(mColours, arrays.mColours, osg::Vec3f(1.0f, 1.0f, 1.0f));

        // Packed where they are copied, and nought — no tangent — where the mesh brought none.
        const auto tangents = mTangents.in(range.mVertices).begin();
        if (arrays.mTangents.empty())
            std::fill_n(tangents, range.mVertices.mCount, 0u);
        else
            std::transform(arrays.mTangents.begin(), arrays.mTangents.end(), tangents, packTangent);
    }

    void MeshTable::notePosed(Index mesh, const osg::BoundingBoxf& bounds)
    {
        MeshRange& range = mRows.at(mesh);
        range.mPosed = true;

        // A pose the size of the last one still reaches somewhere else. An arm that came down is
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
        return mPositions.in(range.mVertices);
    }

    std::span<const std::uint32_t> MeshTable::getMeshIndices(Index mesh) const
    {
        const MeshRange& range = mRows.at(mesh);
        return mIndices.in(range.mIndices);
    }

    std::uint32_t MeshTable::getTriangleCount() const
    {
        return mIndices.size() / 3;
    }

    std::size_t MeshTable::sweep(DeformerTable& deformers)
    {
        const std::size_t freed = mRows.sweep([&](const Index index, MeshRange& range) {
            // The slot stays where it is and only its geometry goes back, because every index
            // above it names a bottom-level acceleration structure that would otherwise be built
            // again. The allocators merge the room with whatever it touches, so a cell leaves as
            // the one hole it came as.
            mVertexRuns.release(range.mVertices);
            mIndexRuns.release(range.mIndices);
            if (range.mSecondTexCoords.mCount > 0)
                mSecondRuns.release(range.mSecondTexCoords);
            deformers.release(range);

            range.mVertices.mCount = 0;
            range.mIndices.mCount = 0;
            range.mSecondTexCoords.mCount = 0;
            range.mUnitStreams = 0;
            range.mBounds = osg::BoundingBoxf();

            // A slot given back names no structure to refit, however it was posed this frame: the
            // structure has gone with it.
            mDeformed.remove(index);

            note(index, SlotNews::Freed);
        });

        // Both sets held a removal per row freed above, and each settles in one pass rather than
        // one per row.
        mDeformed.compact();
        deformers.compact();

        return freed;
    }

    std::size_t MeshTable::getGeometryBytes() const
    {
        return std::size_t{ mPositions.size() } * sizeof(osg::Vec3f)
            + std::size_t{ mNormals.size() } * sizeof(osg::Vec3f)
            + std::size_t{ mTangents.size() } * sizeof(std::uint32_t)
            + std::size_t{ mTexCoords.size() } * sizeof(osg::Vec2f)
            + std::size_t{ mSecondTexCoords.size() } * sizeof(osg::Vec2f)
            + std::size_t{ mColours.size() } * sizeof(osg::Vec3f)
            + std::size_t{ mIndices.size() } * sizeof(std::uint32_t);
    }

    void MeshTable::clearArrivals()
    {
        mChanges.clearArrivals();
    }
}
