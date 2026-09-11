#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <osg/Image>
#include <osg/Vec2i>

#include <components/esm3/refnum.hpp>

#include "cellholds.hpp"
#include "cellplacer.hpp"
#include "cellsupply.hpp"
#include "heldcell.hpp"
#include "preparedcell.hpp"
#include "preparedtexture.hpp"
#include "recycled.hpp"
#include "residency.hpp"
#include "sortedrows.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    class SceneDesc;

    /// The world's cells as this renderer stands them: their ground off the land records, and the
    /// statics of the cells the simulation does not hold as instances of their templates.
    ///
    /// **What `Terrain::QuadTreeWorld` and `Terrain::ObjectPaging` stood as chunks, and why
    /// neither is used.** The quad tree cuts the ground by the eye's distance, nine vertices to a
    /// chunk however wide, and hands the frame a new set of chunks at every level-of-detail
    /// boundary the eye crosses, each folded on the frame that first meets it. The paging merges
    /// every static of a kind inside such a chunk into one geometry, unique to the chunk and folded
    /// the same way — two thirds of the tail a cell ring costs, measured. A ray tracer has no draw
    /// calls to save: a top level takes a thousand instances of one bottom level as one entry
    /// apiece, and a cell's own 65 × 65 grid meets its neighbours vertex for vertex with nothing to
    /// stitch. So the content's own cells are what stands.
    ///
    /// **Two rings, and a thread between them.** The *prepared* ring is the reach plus one band of
    /// cells: each cell in it has been read by the thread — its ground off `Terrain::Storage`, its
    /// references off the content files through `Terrain::ObjectStorage`, its models walked and
    /// folded, its images described — and adopted by the frame into the scene, so its meshes stand
    /// on the device before anything can see them. The *placed* ring is the reach: a cell in it has
    /// its ground in the top level, and outside the active grid its statics too, where the game
    /// stands the real objects. What a frame does is adopt at most one prepared cell and move the
    /// placements of the cells that crossed a boundary.
    ///
    /// **Three objects, because it was three things.** This is the policy: which cells are wanted,
    /// asked for, waited on and adopted. `CellHolds` is the bookkeeping of the models and images the
    /// reader lent and the holds their adoption took. `CellPlacer` is what stands and by what rule.
    ///
    /// **A model's meshes are the frame's own.** A model's drawable is the template's, which is the
    /// one every clone the game makes shares, so a mesh adopted here is the mesh the frame's walk
    /// finds under the clone when the cell becomes active — and the other way round. Nothing is read
    /// twice and nothing is uploaded twice. **The ground's rows are the ring's own**: no drawable
    /// and no state set will ever name them, so the placer holds them on the scene and lets go when
    /// the cell does.
    ///
    /// **A `Residency`, because the sweep is where the frame is.** What this adopts goes through the
    /// extractor's own resolvers, inside the walk that owns the epoch, and what it adopted it holds
    /// by a count on the entry — `Known::mHolds` — so a mesh the ring holds is kept by the same
    /// sweep that keeps a mesh a clone stands on, without being named to it again on every walk.
    ///
    /// **Everything the thread reads is lent and given back.** A cell, a model and an image are the
    /// reader's objects, handed over by address and lent to each cell that names them; a cell the
    /// frame lets go of gives its holds back through the lock, and the reader refills what nothing
    /// holds any more — so a walk across the world reads into the buffers its first cells grew.
    /// `Spares` says why an address and not a shared count, and `giveBackHolds` why a hold is a
    /// cell's.
    class CellRing final : public Residency, public TextureReadings
    {
    public:
        explicit CellRing(SceneDesc& scene);

        /// Stops the thread. A cell in flight is finished and dropped rather than waited out.
        ~CellRing() override;

        CellRing(const CellRing&) = delete;
        CellRing& operator=(const CellRing&) = delete;

        /// Where the world is now, and what of it is read. A world with no content stands nothing,
        /// and a change of what is read drops everything held and starts again.
        void follow(const WorldAround& around) override;

        /// Whether the ring stands the distance's statics at all. The ground stands either way;
        /// off is the A/B `--distant-statics=false` is, and a change of it reads every cell again.
        void setStaticsEnabled(bool enabled);

        /// `CellPlacer::setMinSize`.
        void setMinSize(float minSize) { mPlacer.setMinSize(minSize); }

        /// The frame the next walk is for, so a frame walked twice adopts one cell and not two.
        void setFrame(std::size_t frame);

        /// Whether a walk waits for the cell it is about to adopt.
        ///
        /// **Which frame a cell is adopted on is otherwise the thread's answer**, and a run whose
        /// pictures are compared with another's cannot have that. What makes it the schedule's
        /// answer is the order rather than the wait: one reader takes the cells `ask` sorted and
        /// hands them back in that order, so the cell a frame adopts is the next of that order
        /// whether or not the walk had to wait for it.
        ///
        /// **One cell a frame either way.** Waiting for the whole band and then adopting all of it
        /// would put a region's arrivals on one frame — about ten frames' worth of walk on the
        /// island route — and a route that crosses nineteen times would pay it nineteen times.
        ///
        /// `CompositeQueue::setSettled` is the same rule for the ground's composites.
        void setSettled(bool settled);

        /// `CellPlacer::setReferenceEnabled`.
        void setReferenceEnabled(ESM::RefNum refnum, bool enabled) { mPlacer.setReferenceEnabled(refnum, enabled); }

        void collect(SceneAdopter& into, ExtractionStats& stats) override;

        /// The reading of `image` where a model or a cell's ground the ring holds names it, for the
        /// frame's describe.
        const PreparedTexture* find(const osg::Image& image) const override { return mHolds.find(image); }

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
        int reachInCells() const;

        /// Moves the supply's finished cells into the frame's own list, counting their models. A
        /// cell read with the statics the other way is let go of here.
        void takeDone();

        /// Hands the supply the cells the prepared ring lacks, nearest first.
        void ask(const osg::Vec2i& eye, int band);

        /// Gives back every handed cell the ring must not adopt: one outside the band, and one it
        /// already holds.
        ///
        /// **Run after every `takeDone` and not once a walk.** A cell the reader hands over during
        /// the wait has been through neither the band nor the held test, and `adoptHanded` asks
        /// neither.
        void sift(const osg::Vec2i& eye, int band);

        /// Blocks until the supply has read a cell this walk can adopt. See `setSettled`.
        ///
        /// **Only where the last `ask` named something**, because nothing is coming otherwise and
        /// the reader would never wake this.
        void waitForNext(const osg::Vec2i& eye, int band);

        /// Adopts the next cell the supply read, which is one cell and one frame's worth.
        void adoptHanded(SceneAdopter& into, ExtractionStats& stats);

        void adopt(PreparedCell& cell, SceneAdopter& into, ExtractionStats& stats);

        /// Lets go of a handed cell the frame will not adopt.
        void discard(PreparedCell& cell);

        /// Hands the supply a cell's holds on its models and its ground's textures, for a cell the
        /// frame is letting go of.
        ///
        /// **A hold is a cell's, which is what makes a return exact.** The reader lends a model
        /// once to every cell that names it, and lends it again to cells it reads after the frame
        /// last looked; a return that said "nobody names this" would be wrong the moment such a
        /// cell was read. One that says "this cell is done with it" cannot be, whatever the thread
        /// read in between.
        void giveBackHolds(std::span<PreparedModel* const> models, std::span<PreparedTexture* const> textures);

        void dropCell(HeldCell& cell);
        void dropPlacements();

        /// The whole of what a walk does to the rings: what `collect` wraps in the walk it is inside.
        void walkRings(SceneAdopter& into, ExtractionStats& stats);

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

        bool mStatics = true;
        bool mSettled = false;

        /// Whether the request list has to be rebuilt: the eye's cell, the held and handed sets or
        /// the statics switch changed since it was. `ask` rebuilds the whole band, and nothing else
        /// on the frame path is proportional to the band.
        bool mAskStale = true;
        std::optional<osg::Vec2i> mLastEye;

        std::size_t mFrame = 0;

        /// The frame a cell was last adopted on, so a frame walked twice adopts once.
        std::size_t mAdoptedFrame = ~std::size_t{ 0 };

        /// The cells held, in `CellAt`'s order, and the room a dropped cell's vectors grew.
        SortedRows<HeldCell, osg::Vec2i, CellAt> mCells;
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
