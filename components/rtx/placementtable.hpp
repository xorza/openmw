#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Matrixf>

#include "mesh.hpp"
#include "runs.hpp"
#include "slots.hpp"

namespace Rtx
{
    /// Where every mesh stands, where it stood, and which rows a backend has to write again. A
    /// slot is never moved and never closed up, because a hit reads its slot index back. The two
    /// change lists are the whole of what a backend rewrites: a world is tens of thousands of
    /// placements and a frame changes hundreds. An arrival takes the lowest free slot and never the
    /// last one freed, because the slot settles a tie between coincident sheets and the order a
    /// sweep drops slots in is a hash of node addresses.
    class PlacementTable
    {
    public:
        /// Puts `instance` in a free slot, or in a new one, and returns it. `SceneDesc::addInstance`
        /// is the way in, for the asserts it makes across the tables.
        Index add(const MeshInstance& instance);

        /// Fades the placement in `slot`. Separate from `move`, because an actor fading on the spot
        /// has not moved; a fade that changed the number joins `getMoved` all the same, as a row to
        /// rewrite that carries no motion.
        void fade(Index slot, float opacity);

        /// Moves the placement in `slot`, and says whether that changed anything. A transform equal
        /// to the one already there writes nothing, which is the ordinary case.
        bool move(Index slot, const osg::Matrixf& transform);

        /// Empties `slot`. Its index is not reused until the next `add` asks for one. The slot
        /// joins `getMoved`: a backend has to write its row inactive, or the structure goes on
        /// tracing what stood there.
        void drop(Index slot);

        /// Says `slot`'s row has to be written again for a reason this table did not make — a
        /// material that changed what traversal is told about the surfaces standing on it.
        void rewrite(Index slot) { mMoved.push_back(slot); }

        /// Ends a frame's placement: what moved becomes where things were. Costs what moved and not
        /// what stands. What was moved becomes `getSettled`, and `getMoved` starts empty.
        void advance();

        /// Every slot, standing or empty, in slot order. `MeshInstance::isPlaced` tells them apart.
        std::span<const MeshInstance> getAll() const { return mInstances.getRows(); }

        /// How many slots hold a placement, which is what reaches an acceleration structure.
        std::uint32_t getPlacedCount() const { return mPlacedCount; }

        /// Where each slot stood before the last `advance`, indexed alongside the slots.
        std::span<const osg::Matrixf> getPrevious() const { return mPrevious; }

        /// The slots whose row changed since the last `advance`: placed, moved, faded, dropped, or
        /// wearing a material that changed what traversal is told. A slot can appear more than once.
        std::span<const Index> getMoved() const { return mMoved; }

        /// The slots the last `advance` caught up, whose motion is now still — the other half of
        /// what a backend rewrites, or last frame's motion would stay in the row for ever.
        std::span<const Index> getSettled() const { return mSettled; }

    private:
        SlotRows<MeshInstance> mInstances;
        std::vector<osg::Matrixf> mPrevious;

        /// Plain lists that hold duplicates, where every other change list in this scene is a
        /// `SlotSet`: a slot named twice is a memcpy of a hundred bytes, bounded by the three facts
        /// about a placement that can change in a frame.
        std::vector<Index> mMoved;
        std::vector<Index> mSettled;

        std::uint32_t mPlacedCount = 0;
    };
}
