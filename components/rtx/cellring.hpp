#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <boost/container/flat_set.hpp>
#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

#include "cellplacer.hpp"
#include "cellsupply.hpp"
#include "extractionstats.hpp"
#include "held.hpp"
#include "prepared.hpp"
#include "residency.hpp"
#include "scratch.hpp"
#include "slots.hpp"
#include "stepped.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    class SceneDesc;

    /// The world's cells as this renderer stands them: their ground off the land records, and the
    /// statics of the cells the simulation does not hold as instances of their templates. Not
    /// `Terrain::QuadTreeWorld`'s chunks nor `Terrain::ObjectPaging`'s merges, which cut and fold
    /// geometry by the eye's distance on the frame that first meets it — two thirds of the tail a
    /// cell ring costs, measured. A ray tracer has no draw calls to save: a top level takes a
    /// thousand instances of one bottom level as one entry apiece, and a cell's own 65 × 65 grid
    /// meets its neighbours vertex for vertex.
    ///
    /// Two rings, and a thread between them. The *prepared* ring is the reach plus one band: each
    /// cell in it has been read by the thread and adopted by the frame into the scene, so its
    /// meshes stand on the device before anything can see them. The *placed* ring is the reach: a
    /// cell in it has its ground in the top level, and outside the active grid its statics too. A
    /// frame adopts at most one prepared cell and moves the placements of the cells that crossed a
    /// boundary. This is the policy; `CellHolds` is the bookkeeping of what the reader lent and
    /// the holds adoption took; `CellPlacer` is what stands and by what rule. A model's drawable
    /// is the template's, so a mesh adopted here is the mesh the frame's walk finds under the
    /// clone when the cell becomes active, and nothing is uploaded twice. The ground's rows are
    /// the ring's own: no drawable will ever name them, so the placer holds them on the scene. What
    /// this adopts goes through the extractor's own resolvers inside the walk (`SceneAdopter`),
    /// and holds by `Known::mHolds` rather than being named again on every walk. Everything the
    /// thread reads is lent and given back — `Spares` says why an address and not a shared count,
    /// and `giveBackHolds` why a hold is a cell's.
    ///
    /// **The lamps of the cells the game has not loaded are the ring's too.** `REC_LIGH` is not a
    /// paged type and must not become one, because both renderers read the paging's type filter;
    /// what this fork cannot keep is the light, because rays go everywhere, and a town four cells
    /// away that goes dark at dusk is the world stating something the content files do not. Read
    /// with the cell's other records in one walk of them, off the frame, and stood by
    /// `CellPlacer::place`, which says where.
    class CellRing
    {
    public:
        explicit CellRing(SceneDesc& scene);

        CellRing(const CellRing&) = delete;
        CellRing& operator=(const CellRing&) = delete;

        /// Stops the thread. A cell in flight is finished and dropped rather than waited out.
        ~CellRing();

        /// Where the world is now, and what of it is read. A world with no content stands nothing,
        /// and a change of what is read drops everything held and starts again. Told once a frame,
        /// before the walk.
        void follow(const WorldAround& around);

        /// Whether the ring stands the distance's statics at all. The ground stands either way;
        /// off is the A/B `--distant-statics=false` is, and a change of it reads every cell again.
        void setStaticsEnabled(bool enabled);

        void setMinSize(float minSize) { mPlacer.setMinSize(minSize); }

        /// The frame the next walk is for, so a frame walked twice adopts one cell and not two.
        /// Told by `SceneExtractor::extractWorld`, which is the call that has the ring and the
        /// frame both; a caller that drives a ring outside a walk says it itself.
        void setFrame(std::size_t frame);

        /// Whether a walk waits for the cell it is about to adopt, so which frame a cell is adopted
        /// on is the schedule's answer and not the thread's. The order is what makes it so: one
        /// reader takes the cells `ask` sorted and hands them back in that order. One cell a frame
        /// either way.
        void setSettled(bool settled);

        /// `CellPlacer::setReferenceEnabled`, `blacklistReference` and `forgetReferences`, over the
        /// cells held.
        void setReferenceEnabled(ESM::RefNum refnum, bool enabled);
        void blacklistReference(ESM::RefNum refnum);
        void forgetReferences();

        /// Hands `into` everything held that the graph does not parent, and adds what it stood to
        /// `stats` — the walk's own, because the ring is stood inside the walk.
        void collect(SceneAdopter& into, ExtractionStats& stats);

        /// Gives `into` back every hold `forget` let go of, for a ring the frame will not walk
        /// again — a world detached. `collect` does the same at both ends of a walk.
        void releaseHolds(SceneAdopter& into) { mHolds.releaseParts(into); }

        /// Appends the reference number of every static standing in the top level, for a check
        /// that asks the game whether it stands the same one.
        void collectStanding(std::vector<ESM::RefNum>& into) const;

        // Read by the tests and by nothing else.
        /// How many cells the prepared ring holds.
        std::size_t getHeldCellCount() const { return mCells.size(); }

    private:
        /// What the cell table is ordered by, stated once so that no search can disagree with the
        /// insertion it is looking for. `osg::Vec2i` orders lexicographically already, which is the
        /// order a walk over the held cells wants: the same walk on every machine.
        struct CellAt
        {
            const osg::Vec2i& operator()(const HeldCell& held) const { return held.mCell; }
        };

        bool holds(const osg::Vec2i& cell) const;
        bool handed(const osg::Vec2i& cell) const;

        /// Moves the supply's finished cells into the frame's own list, counting their models. A
        /// cell read with the statics the other way is let go of here.
        void takeDone();

        /// Hands the supply the cells the prepared disc lacks, nearest first. `band` is the disc's
        /// radius in units, which is the reach and the prepared band past it.
        void ask(const osg::Vec3f& eye, float band);

        /// Gives back every handed cell the ring must not adopt: one outside the band, and one it
        /// already holds. Run after every `takeDone`, because a cell handed over during the wait
        /// has been through neither test.
        void sift(const osg::Vec3f& eye, float band);

        /// Blocks until the supply has read a cell this walk can adopt (`setSettled`). Only where
        /// the last `ask` named something, because nothing is coming otherwise and the reader would
        /// never wake this.
        void waitForNext(const osg::Vec3f& eye, float band);

        /// Adopts the next cell the supply read, which is one cell and one frame's worth.
        void adoptHanded(SceneAdopter& into, ExtractionStats& stats);

        void adopt(PreparedCell& cell, SceneAdopter& into, ExtractionStats& stats);

        /// Lets go of a handed cell the frame will not adopt.
        void discard(PreparedCell& cell);

        /// Hands the supply a cell's holds on its models and its ground's textures, for a cell the
        /// frame is letting go of — one it adopted, or one handed over that it never did. A hold
        /// is a cell's, which is what makes a return exact whatever the thread read in between.
        void giveBackHolds(const HeldCell& cell);
        void giveBackHolds(const PreparedCell& cell);

        void dropCell(HeldCell& cell);
        void dropPlacements();

        /// The whole of what a walk does to the rings: what `collect` wraps in the walk it is inside.
        void walkRings(SceneAdopter& into, ExtractionStats& stats);

        /// Whether the top level holds exactly what the held cells say it should, cell by cell and
        /// in total — `CellPlacer::standsAsHeld` and `standsNoMore`. Asserted after every walk, so
        /// a placement that outlived its cell, or a cell whose placement went missing, is found on
        /// the frame it happened rather than seen at the horizon.
        bool standsAsHeld() const;

        /// Lets go of every cell, every model and every image the frame holds. What the supply lent
        /// dies with its reader, so this runs before the supply is pointed anywhere else.
        void forget();

        /// The cells themselves, read on a thread of its own.
        CellSupply mSupply;

        /// The models and images lent, and what they were adopted as.
        CellHolds mHolds;

        /// What of the held cells stands, and by what rule.
        CellPlacer mPlacer;

        /// Where the eye is and how much world there is around it, as the last `follow` said.
        WorldAround mAround;

        /// Where the ring stands in a frame: told where the world is, or walked. `follow` comes
        /// before `collect` — "told once a frame, before the walk" — and a walk that was not told
        /// is asserted where it happens rather than standing last frame's rings.
        enum class Turn
        {
            Followed,
            Collected,
        };

        Stepped<Turn> mTurn{ Turn::Collected };

        bool mStatics = true;
        bool mSettled = false;

        /// Whether the request list has to be rebuilt: the eye, the held and handed sets or the
        /// statics switch changed since it was. `ask` rebuilds the whole band, and nothing else on
        /// the frame path is proportional to the band.
        bool mAskStale = true;
        std::optional<osg::Vec3f> mLastEye;

        std::size_t mFrame = 0;

        /// The frame a cell was last adopted on, so a frame walked twice adopts once.
        std::size_t mAdoptedFrame = ~std::size_t{ 0 };

        /// The cells held, in `CellAt`'s order, and the room a dropped cell's vectors grew.
        boost::container::flat_set<HeldCell, KeyedLess<osg::Vec2i, CellAt>, std::vector<HeldCell>> mCells;
        Recycled<HeldCell> mSpareCells;

        /// What the thread read and handed over, and the frame has not adopted yet, in the order it
        /// arrived.
        std::vector<PreparedCell*> mHanded;

        // Refilled per frame.
        std::vector<PreparedCell*> mDoneScratch;

        /// What the supply is asked for, refilled per walk and never freed.
        CellRequest mAsking;
    };
}
