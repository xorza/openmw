#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include "deformertable.hpp"
#include "index.hpp"
#include "mesharrays.hpp"
#include "meshrange.hpp"
#include "runbuffer.hpp"
#include "shaders/scene.h"
#include "shapefold.hpp"
#include "slotchanges.hpp"
#include "slotrows.hpp"
#include "slotset.hpp"

namespace Rtx
{
    /// Every mesh the scene holds, and the shared buffers its triangles live in.
    ///
    /// **One type, because a row and the runs behind it are one invariant.** A mesh slot is one row
    /// and its geometry is as long as the model, so the two are freed by different mechanisms — a
    /// free list and two allocators — and a mesh that deforms has to give its deformer back between
    /// them.
    ///
    /// **The deformers are borrowed and not owned.** A rig is shared by every mesh built from one
    /// skin, so the count lives with the table that hands rigs out; this stands one as a mesh
    /// arrives and releases one as a mesh goes.
    class MeshTable
    {
    public:
        /// How many vertices one block of the vertex attribute buffers holds, and how many indices
        /// one block of the index buffer does.
        ///
        /// **The shaders' own numbers**, because a run this places against a block is resolved back
        /// to that block by a shader dividing by the same figure. `Shaders::VERTEX_BLOCK` says at
        /// length what they are for.
        static constexpr Index sVertexBlock = Shaders::VERTEX_BLOCK;
        static constexpr Index sIndexBlock = Shaders::INDEX_BLOCK;

        explicit MeshTable(DeformerTable& deformers)
            : mDeformers(deformers)
        {
        }

        std::size_t size() const { return mRows.size(); }

        /// How many slots hold a mesh, which is what a sweep compares its survivors against.
        std::size_t getLiveCount() const { return mRows.getLiveCount(); }

        /// Copies the vertex data into the shared buffers and returns the new mesh's index.
        ///
        /// Throws where the mesh is longer than a block. **Named rather than asserted**, because a
        /// vertex count comes out of a content file and a run that straddled a block would be
        /// written across two device allocations that are not next to each other.
        Index add(const MeshArrays& arrays, FoldedShape shape, Deform deform, Index deformer, Index material);

        /// What a pose that changed does beside its rows: the reach, and the mesh named for the
        /// frame, once.
        void notePosed(Index mesh, const osg::BoundingBoxf& bounds);

        /// Notes every slot a sweep must not free, and says how many distinct ones `keep` named.
        std::size_t mark(std::span<const Index> keep);

        /// Frees every slot the last `mark` did not name, and says how many that was.
        std::size_t sweep();

        std::span<const osg::Vec3f> getPositions() const { return mPositions.getAll(); }
        std::span<const osg::Vec3f> getNormals() const { return mNormals; }
        std::span<const osg::Vec2f> getTexCoords() const { return mTexCoords; }
        std::span<const osg::Vec3f> getColours() const { return mColours; }
        std::span<const std::uint32_t> getIndices() const { return mIndices.getAll(); }
        std::span<const MeshRange> getRows() const { return mRows.getRows(); }

        std::span<const osg::Vec3f> getMeshPositions(Index mesh) const;
        std::span<const std::uint32_t> getMeshIndices(Index mesh) const;

        /// Which meshes changed shape since the last `clearDeformed`, each named once.
        std::span<const Index> getDeformed() const { return mDeformed.getSlots(); }
        void clearDeformed() { mDeformed.clear(); }

        std::span<const Index> getArrived() const { return mChanges.getArrived(); }
        std::span<const Index> getFreed() const { return mChanges.getFreed(); }

        /// How many times a mesh has appeared, whether at the end of the table or into a slot
        /// something else left.
        std::uint64_t getRevision() const { return mRevision; }

        std::uint32_t getTriangleCount() const;
        std::size_t getGeometryBytes() const;

        void clearArrivals();

    private:
        /// Makes the attribute buffers as long as the positions are and writes `range`'s run of
        /// each. Fills one the mesh did not bring with what stands for nothing there, because a
        /// reused slot still holds its last tenant's.
        void writeAttributes(const MeshRange& range, const MeshArrays& arrays);

        /// Records `slot` as having arrived or gone, and grows the list to reach it.
        void note(Index slot, SlotNews what);

        DeformerTable& mDeformers;

        /// Where a mesh's vertices and its indices live.
        ///
        /// **Runs and not slots**, which is why these are `RunBuffer`s and `mRows` is not: a mesh
        /// slot is one row of a table, but the geometry behind it is as long as the model. A list
        /// of slots cannot give a variable length back.
        ///
        /// **One buffer holds the run and three follow it.** The position, normal,
        /// texture-coordinate and colour arrays are parallel and a vertex id indexes all four, so
        /// the three below are as long as the positions are and are never asked an allocator's
        /// question — `writeAttributes` is where that rule lives.
        RunBuffer<osg::Vec3f> mPositions{ sVertexBlock };
        RunBuffer<std::uint32_t> mIndices{ sIndexBlock };

        std::vector<osg::Vec3f> mNormals;
        std::vector<osg::Vec2f> mTexCoords;

        /// The per-vertex colour, in linear light. **White where a mesh brought none**, so that a
        /// hit multiplies by it whatever the content said and no shader branches on whether there
        /// is one. `MeshArrays::mColours` says why it is linear here.
        std::vector<osg::Vec3f> mColours;

        /// Every mesh row, and the slots nothing stands in.
        ///
        /// **Slots are never moved and never closed up**, because what goes on the free list is
        /// what one departing ring left — tens of entries, not the table. A slot that is taken over
        /// keeps its index and every placement standing on it stays where it is.
        SlotRows<MeshRange> mRows;

        /// Which meshes were posed this frame. Emptied with the placement rather than with the
        /// arrivals: a pose is a fact about the frame and an arrival is a fact about the scene.
        SlotSet mDeformed;

        /// Which slots arrived and which were freed, since a backend last read them.
        SlotChanges mChanges;

        /// How many times a mesh has appeared. `SceneDesc::getStructureRevision` says what it is
        /// read for and why a texture arriving is counted apart from it.
        std::uint64_t mRevision = 0;
    };
}
