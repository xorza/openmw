#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec2i>

#include <components/esm3/refnum.hpp>

#include "cellworld.hpp"
#include "extractionstats.hpp"
#include "held.hpp"
#include "material.hpp"
#include "specularlayout.hpp"

namespace Rtx
{
    class CellHolds;
    class SceneDesc;
    struct PreparedCell;

    /// Where the cells the ring holds stand: which of their placements are in the top level, by
    /// the rings and the size rule, and how their ground shades, by the grid. `CellRing` decides
    /// what is prepared and adopted; this decides what of it stands. The ground's rows are adopted
    /// here too, because the ground is the one thing this stands that no model was read for.
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

        /// `[RTX] specular map layout`: whether a ground layer's `_diffusespec` is an authored
        /// albedo with its roughness in alpha, or the plain diffuse OpenMW swaps in. Told rather
        /// than asked, for the same reason.
        void setSpecularLayout(SpecularLayout layout) { mSpecularLayout = layout; }

        /// What the game says of one reference, which the content files cannot: a script has
        /// disabled it, or enabled it again. Applied to the cells `held` at once, because `place`
        /// walks only what the size rule changed since the last frame. Remembered for the cells not
        /// yet held, which arrive with the flag set.
        void setReferenceEnabled(ESM::RefNum refnum, bool enabled, std::span<HeldCell> held);

        /// The game moved, deleted or animates the reference, so it is never stood again whatever
        /// a script says of it afterwards: upstream's paging keeps the same list beside its
        /// disabled one, and a `setReferenceEnabled(refnum, true)` does not undo it. Dropped from
        /// the cells `held` at once, and kept out of the cells not yet held.
        void blacklistReference(ESM::RefNum refnum, std::span<HeldCell> held);

        /// Forgets everything a script said and everything the game blacklisted: the world is
        /// cleared for a new game or a saved one, and what was kept out of the old one stands in
        /// the new. Every reference kept out of the cells `held` stands again at once, as
        /// `setReferenceEnabled` would stand each.
        void forgetReferences(std::span<HeldCell> held);

        /// Adopts a cell's ground into the scene, on rows held on the scene. `around` says whether
        /// it shades from its stack.
        void adoptGround(const PreparedCell& cell, HeldCell& held, const WorldAround& around, ExtractionStats& stats);

        /// Fills `held.mPlacements` from the cell's references, one per part of each model as
        /// `holds` adopted it, disabled where a script said so, and sorted for `place`; and
        /// `held.mLights` from the cell's lamps, whole.
        void adoptPlacements(const PreparedCell& cell, HeldCell& held, CellHolds& holds);

        /// Lets a cell's ground go: its slot and its rows. The sweep after this walk is what frees
        /// the rows.
        void dropGround(HeldCell& cell);

        /// Takes a cell's placements out of the top level, keeping the cell.
        void dropSlots(HeldCell& cell);

        /// Places and drops one cell by the reach and the size rule, flattens its ground by the
        /// grid, and stands its lamps where the cell is in reach and outside the grid — the game's
        /// own graph carries the lamps inside it, and a lantern must not be counted twice.
        /// @return how many lamps were stood.
        std::uint32_t place(HeldCell& cell, const WorldAround& around);

        /// How many statics and how many grounds stand in the top level.
        std::uint32_t getPlaced() const { return mPlaced; }
        std::uint32_t getGroundPlaced() const { return mGroundPlaced; }

        /// Whether `cell` stands exactly what its holding says, by the rings and the size rule as
        /// `around` stands now: every placement the rule admits and no script disabled has a slot,
        /// and that slot holds its mesh and material under the ring's own name; every other
        /// placement has none; and the ground stands in the reach and nowhere else. What `place`
        /// keeps, asked after it, for an assert.
        bool standsAsHeld(const HeldCell& cell, const WorldAround& around) const;

        /// Whether the top level holds exactly `getPlaced() + getGroundPlaced()` slots under the
        /// ring's name: a slot the ring stood and lost track of would stand for ever, and only a
        /// count of the table can see one.
        bool standsNoMore() const;

    private:
        /// Whether a script or the blacklist keeps the reference down.
        bool isDisabled(ESM::RefNum refnum) const;

        /// `setPlacementEnabled` over every placement of the reference in the cells `held`.
        void setHeldEnabled(ESM::RefNum refnum, bool enabled, std::span<HeldCell> held);

        /// What a script's word does to one placement: the flag, and the slot where the size rule
        /// has the placement `shown`.
        void setPlacementEnabled(Placement& placement, bool shown, bool enabled);

        /// Puts `stood` in the top level under the ring's name and counts it in `standing`, and
        /// takes it out again. Dropping what does not stand is nothing.
        void stand(Stood& stood, std::uint32_t& standing);
        void drop(Stood& stood, std::uint32_t& standing);

        /// Whether a cell's ground wants its stack flattened where the eye stands now.
        static bool wantsFlattening(const osg::Vec2i& cell, const HeldGround& ground, const WorldAround& around);

        SceneDesc& mScene;

        float mMinSize = 0.0f;

        SpecularLayout mSpecularLayout = SpecularLayout::Ignore;

        /// References a script has disabled, sorted.
        std::vector<ESM::RefNum> mDisabled;

        /// References the game blacklisted, sorted. Apart from the disabled, because a script may
        /// enable one of those again and never one of these.
        std::vector<ESM::RefNum> mBlacklisted;

        std::uint32_t mPlaced = 0;
        std::uint32_t mGroundPlaced = 0;

        // Refilled per ground adopted.
        std::vector<MaterialLayer> mLayerScratch;
    };
}
