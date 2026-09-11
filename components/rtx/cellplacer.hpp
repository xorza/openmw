#pragma once

#include <cstdint>
#include <vector>

#include <osg/Vec2i>

#include <components/esm3/refnum.hpp>

#include "extractionstats.hpp"
#include "heldcell.hpp"
#include "material.hpp"
#include "residency.hpp"

namespace Rtx
{
    class CellHolds;
    class SceneDesc;
    struct PreparedCell;

    /// Where the cells the ring holds stand: which of their placements are in the top level, by
    /// the rings and the size rule, and how their ground shades, by the grid.
    ///
    /// **The placement half of the cell ring, apart from the policy that decides which cells are
    /// held.** `CellRing` decides what is prepared and adopted; this decides what of it stands, and
    /// owns the counts of what does. The ground's rows are adopted here too, because the ground is
    /// the one thing this stands that no model was read for.
    class CellPlacer
    {
    public:
        explicit CellPlacer(SceneDesc& scene)
            : mScene(scene)
        {
        }

        /// The size rule's constant: a reference is placed while its scaled radius is at least this
        /// times the eye's distance to its cell. `object paging min size`, told rather than asked
        /// because this library reads no settings.
        void setMinSize(float minSize) { mMinSize = minSize; }

        /// What the game says of one reference, which the content files cannot: a script has
        /// disabled it, or enabled it again. Placed or dropped on the next `place`.
        void setReferenceEnabled(ESM::RefNum refnum, bool enabled);

        /// Adopts a cell's ground into the scene, on rows held on the scene, and holds its textures
        /// on `holds` for the frame's describe. `around` says whether it shades from its stack.
        void adoptGround(const PreparedCell& cell, HeldCell& held, CellHolds& holds, const WorldAround& around,
            ExtractionStats& stats);

        /// Lets a cell's ground go: its slot, its texture holds and its rows. The sweep after this
        /// walk is what frees the rows.
        void dropGround(HeldCell& cell, CellHolds& holds);

        /// Takes a cell's placements out of the top level, keeping the cell.
        void dropSlots(HeldCell& cell);

        /// Places and drops one cell by the rings and the size rule, and flattens its ground by the
        /// grid. `eye` and `reach` are `around`'s, in cells, worked out once by the caller.
        void place(HeldCell& cell, const WorldAround& around, const osg::Vec2i& eye, int reach);

        /// How many statics and how many grounds stand in the top level.
        std::uint32_t getPlaced() const { return mPlaced; }
        std::uint32_t getGroundPlaced() const { return mGroundPlaced; }

    private:
        static bool inActiveGrid(const osg::Vec2i& cell, const WorldAround& around);
        bool isDisabled(ESM::RefNum refnum) const;

        /// Whether a cell's ground wants its stack flattened where the eye stands now.
        static bool wantsFlattening(const osg::Vec2i& cell, const HeldGround& ground, const WorldAround& around);

        SceneDesc& mScene;

        float mMinSize = 0.0f;

        /// References a script has disabled, sorted.
        std::vector<ESM::RefNum> mDisabled;

        std::uint32_t mPlaced = 0;
        std::uint32_t mGroundPlaced = 0;

        // Refilled per ground adopted.
        std::vector<MaterialLayer> mLayerScratch;
    };
}
