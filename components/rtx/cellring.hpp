#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <osg/Image>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

#include "cellsupply.hpp"
#include "index.hpp"
#include "material.hpp"
#include "mirroridentity.hpp"
#include "preparedcell.hpp"
#include "preparedtexture.hpp"
#include "residency.hpp"
#include "sortedrows.hpp"
#include "texturebuilder.hpp"

namespace Terrain
{
    class Storage;
}

namespace Rtx
{
    class ContentSource;
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
    /// stands the real objects. What a frame does is adopt at most one prepared cell, move the
    /// placements of the cells that crossed a boundary, and stamp what stands.
    ///
    /// **A model's meshes are the frame's own.** A model's drawable is the template's, which is the
    /// one every clone the game makes shares, so a mesh adopted here is the mesh the frame's walk
    /// finds under the clone when the cell becomes active — and the other way round. Nothing is read
    /// twice and nothing is uploaded twice. **The ground's rows are the ring's own**: no drawable
    /// and no state set will ever name them, so the ring names them to the extractor on every walk
    /// and says when it lets one go.
    ///
    /// **A `Residency`, because the sweep is where the frame is.** What this stamps and adopts goes
    /// through the extractor's own resolvers, inside the walk that owns the epoch, so a mesh the
    /// ring holds is kept by the same rule that keeps a mesh a clone stands on.
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

        /// Where the ground and the models come from, and which nodes a walk of a template may
        /// descend into.
        ///
        /// **Apart from `follow`, because only this residency has them.** What both residencies
        /// share is `WorldAround`; a reader also needs the land, the loader and the frame walk's own
        /// mask, and those are nobody else's to be told.
        void setContent(Terrain::Storage* ground, ContentSource* content, osg::Node::NodeMask mask);

        /// Where the world is now. A world with no content stands nothing, and a change of what is
        /// read drops everything held and starts again.
        void follow(const WorldAround& around) override;

        /// Whether the ring stands the distance's statics at all. The ground stands either way;
        /// off is the A/B `--distant-statics=false` is, and a change of it reads every cell again.
        void setStaticsEnabled(bool enabled);

        /// The size rule's constant: a reference is placed while its scaled radius is at least this
        /// times the eye's distance to its cell. `object paging min size`, told rather than asked
        /// because this library reads no settings.
        void setMinSize(float minSize);

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
        /// **One cell a frame either way, which is the rule a settled walk used to break.** Waiting
        /// for the whole band and then adopting all of it put a region's arrivals on one frame:
        /// measured on `island-crossing`, `walk ms` read 59.6 ms worst against 5.5 ms with this off,
        /// and a route that crosses nineteen times paid it nineteen times.
        ///
        /// `CompositeQueue::setSettled` is the same rule for the ground's composites.
        void setSettled(bool settled);

        /// What the game says of one reference, which the content files cannot: a script has
        /// disabled it, or enabled it again. Placed or dropped on the next walk.
        void setReferenceEnabled(ESM::RefNum refnum, bool enabled);

        ResidencyCount collect(Collector& into) override;

        /// The reading of `image` where a model or a cell's ground the ring holds names it, for the
        /// frame's describe.
        const PreparedTexture* find(const osg::Image& image) const override;

        /// How many cells the prepared ring holds.
        std::size_t getHeldCellCount() const { return mCells.size(); }

    private:
        /// One placement the ring may stand: a part of a model at a reference.
        struct Placement
        {
            Index mMesh = sNoIndex;
            Index mMaterial = sNoIndex;
            osg::Matrixf mTransform;
            float mRadius = 0.0f;
            ESM::RefNum mRefNum;

            /// The slot it stands in, or none while the size rule or the ring keeps it out.
            Index mSlot = sNoIndex;
        };

        /// A cell's ground as the frame holds it: its rows, where it stands, and what shades it.
        ///
        /// **Its own type, because the seven are empty together.** They were seven fields of the
        /// cell and `mGroundMesh == sNoIndex` stood for all of them — one rule a reader had to know
        /// rather than a shape that states it. `PreparedGround::mStands` says the same thing one
        /// step earlier.
        struct HeldGround
        {
            /// The rows the ring owns, which no drawable and no state set will ever name.
            Index mMesh = sNoIndex;
            Index mMaterial = sNoIndex;

            /// The slot it stands in, or none while the ring keeps it out.
            Index mSlot = sNoIndex;

            /// The cell's centre, which the mesh's own positions are relative to.
            osg::Vec3f mOrigin;

            /// How many layers the stack holds, which is whether a composite is worth asking for at
            /// all.
            std::uint32_t mLayers = 0;

            /// Whether the material row asks for a composite now, so a crossing of the active grid
            /// rewrites it once.
            bool mFlattened = false;

            /// The readings of its textures, held for as long as the cell is.
            std::vector<PreparedTexture*> mTextures;

            /// Empties it for the next cell, keeping the room the texture list grew.
            void reuse()
            {
                mMesh = sNoIndex;
                mMaterial = sNoIndex;
                mSlot = sNoIndex;
                mLayers = 0;
                mFlattened = false;
                mTextures.clear();
            }
        };

        /// A cell the frame has adopted. Its vectors are kept when it is dropped, so a cell that
        /// arrives later refills them rather than growing new ones.
        struct HeldCell
        {
            osg::Vec2i mCell;

            /// Whether its references were read, so a cell read under the other setting is dropped
            /// and read again.
            bool mStatics = false;

            std::vector<Placement> mPlacements;
            std::vector<PreparedModel*> mModels;

            /// The ground, or nothing where the land names none.
            ///
            /// **The room its texture list grew is kept across a drop**, which is what `mSpare`
            /// takes a cell for — so what is emptied here is the optional's contents and never the
            /// optional itself.
            std::optional<HeldGround> mGround;
        };

        /// One part of a model as the scene holds it, and the entries the walk stamps it by.
        struct AdoptedPart
        {
            Index mMesh = sNoIndex;
            Index mMaterial = sNoIndex;

            /// Into the extractor's identity maps, which hold their entries in place. Stamped every
            /// frame, which is what keeps the sweep from erasing either.
            Known* mMeshEntry = nullptr;
            Known* mMaterialEntry = nullptr;
        };

        /// A model the frame knows of: named by a cell it holds or by one it is about to adopt.
        ///
        /// **Both counts, because the frame forgets a model only when neither names it** — its
        /// parts are adopted once, and its images are found for as long as it is known. What the
        /// reader holds the model for is a different count and the reader's own: `giveBackHolds`.
        struct HeldModel
        {
            PreparedModel* mModel = nullptr;

            /// Empty until the first cell naming the model is adopted.
            std::vector<AdoptedPart> mParts;

            std::uint32_t mHeld = 0;
            std::uint32_t mPending = 0;
        };

        /// An image some known model or held ground names, and its reading, for `find`.
        struct HeldTexture
        {
            const osg::Image* mImage = nullptr;
            const PreparedTexture* mTexture = nullptr;
            std::uint32_t mHolders = 0;
        };

        /// What each of the three tables is ordered by, stated once each so that no search can
        /// disagree with the insertion it is looking for.
        struct CellAt
        {
            /// `osg::Vec2i` orders lexicographically already, which is the order a walk over the
            /// held cells wants: the same walk on every machine.
            const osg::Vec2i& operator()(const HeldCell& held) const { return held.mCell; }
        };

        struct ModelAt
        {
            const PreparedModel* operator()(const HeldModel& held) const { return held.mModel; }
        };

        struct TextureAt
        {
            const osg::Image* operator()(const HeldTexture& held) const { return held.mImage; }
        };

        bool inActiveGrid(const osg::Vec2i& cell) const;
        bool holds(const osg::Vec2i& cell) const;
        bool pending(const osg::Vec2i& cell) const;
        bool isDisabled(ESM::RefNum refnum) const;
        int reachInCells() const;

        /// Whether a cell's ground wants its stack flattened where the eye stands now.
        bool wantsFlattening(const osg::Vec2i& cell, const HeldGround& ground) const;

        /// Counts one more holder of `texture`'s reading, for `find`.
        void holdTexture(const PreparedTexture& texture);

        /// Counts one holder off, and forgets the reading where none is left.
        void dropTexture(const PreparedTexture& texture);

        /// Moves the supply's finished cells into the frame's own list, counting their models. A
        /// cell read with the statics the other way is let go of here.
        void takeDone();

        /// Hands the supply the cells the prepared ring lacks, nearest first.
        void ask(const osg::Vec2i& eye, int band);

        /// Gives back every pending cell the ring must not adopt: one outside the band, and one it
        /// already holds.
        ///
        /// **Run after every `takeDone` and not once a walk.** A cell the reader hands over during
        /// the wait has been through neither the band nor the held test, and `adoptPending` asks
        /// neither.
        void sift(const osg::Vec2i& eye, int band);

        /// Blocks until the supply has read a cell this walk can adopt. See `setSettled`.
        ///
        /// **Only where the last `ask` named something**, because nothing is coming otherwise and
        /// the reader would never wake this.
        void waitForNext(const osg::Vec2i& eye, int band);

        /// Adopts the next cell the supply read, which is one cell and one frame's worth.
        void adoptPending();

        void adopt(PreparedCell& cell);

        /// Adopts a cell's ground into the scene, on rows the ring owns.
        void adoptGround(const PreparedCell& cell, HeldCell& held);

        /// Lets go of a pending cell the frame will not adopt.
        void discard(PreparedCell& cell);

        /// The entry for `model`, made where the frame knows of none.
        HeldModel& know(PreparedModel& model);

        /// The entry for a model the frame knows of.
        HeldModel& knownOf(const PreparedModel& model);

        /// Adopts a model's parts into the scene, where a cell first stands it.
        void adoptParts(HeldModel& held);

        /// Counts one cell of `model` off the frame's own count, and forgets the model where none
        /// is left. The cell's hold on it goes back to the reader on its own: `giveBackHolds`.
        void release(PreparedModel& model, bool wasHeld);

        /// Hands the supply a cell's holds on its models and its ground's textures, for a cell the
        /// frame is letting go of.
        ///
        /// **A hold is a cell's, which is what makes a return exact.** The reader lends a model
        /// once to every cell that names it, and lends it again to cells it reads after the frame
        /// last looked; a return that said "nobody names this" would be wrong the moment such a
        /// cell was read. One that says "this cell is done with it" cannot be, whatever the thread
        /// read in between.
        void giveBackHolds(std::span<PreparedModel* const> models, std::span<PreparedTexture* const> textures);

        /// Takes a cell's placements out of the top level, keeping the cell.
        void dropSlots(HeldCell& cell);

        void dropCell(HeldCell& cell);
        void dropPlacements();

        /// Places and drops by the rings and the size rule, and flattens by the grid.
        void place(const osg::Vec2i& eye, int reach);

        /// Says the walk met every mesh and material the ring holds.
        void stamp();

        /// What this stood and what it let go of, with the tally started again and the walk
        /// forgotten. **Every path out of `collect` ends here**, which is what keeps `mInto` from
        /// outliving the call it belongs to.
        ResidencyCount report();

        /// The walk in progress. **Asserted rather than tested**: everything that reaches it is
        /// inside `collect`, and a call from anywhere else is this class's own mistake.
        Collector& walk() const;

        /// Lets go of every cell, every model and every image the frame holds. What the supply lent
        /// dies with its reader, so this runs before the supply is pointed anywhere else.
        void forget();

        SceneDesc& mScene;

        /// The cells themselves, read on a thread of its own.
        CellSupply mSupply;

        /// Where the eye is and how much world there is around it, as the last `follow` said.
        WorldAround mAround;

        /// The walk this residency is inside, or null between walks.
        ///
        /// **Held rather than handed down, which is what `MirrorPass` does for the same shape.** A
        /// walk is not re-entrant — `collect` is the only thing here that adopts or stamps — and of
        /// the three methods that reached it, two carried a parameter they only forwarded.
        Collector* mInto = nullptr;

        /// The three only this residency needs. See `setContent`.
        Terrain::Storage* mGround = nullptr;
        ContentSource* mContent = nullptr;
        osg::Node::NodeMask mMask = ~0u;

        bool mStatics = true;
        float mMinSize = 0.0f;
        bool mSettled = false;

        std::size_t mFrame = 0;

        /// The frame a cell was last adopted on, so a frame walked twice adopts once.
        std::size_t mAdoptedFrame = ~std::size_t{ 0 };

        /// The three tables the ring is asked about, each in the order its own `*At` states.
        ///
        /// **Kept when an entry goes**, which is what the spares beside two of them are for: a cell
        /// and a model each own vectors, and a row taken back out is room the next one refills
        /// rather than a heap call on the frame a cell lands.
        SortedRows<HeldCell, osg::Vec2i, CellAt> mCells;
        std::vector<HeldCell> mSpareCells;

        SortedRows<HeldModel, const PreparedModel*, ModelAt> mModels;
        std::vector<HeldModel> mSpareModels;

        SortedRows<HeldTexture, const osg::Image*, TextureAt> mTextures;

        /// What the thread read and the frame has not adopted yet, in the order it arrived.
        std::vector<PreparedCell*> mPending;

        /// References a script has disabled, sorted.
        std::vector<ESM::RefNum> mDisabled;

        std::uint32_t mPlaced = 0;
        std::uint32_t mGroundPlaced = 0;

        /// What this residency has stood and let go of, reported at the end of every `collect`.
        ///
        /// **The disowned halves survive between walks**, because a world that is detached drops
        /// every cell outside any walk — and the sweep still has to be told those rows are gone.
        ResidencyCount mCount;

        // Refilled per frame.
        std::vector<PreparedCell*> mDoneScratch;
        std::vector<MaterialLayer> mLayerScratch;

        /// What the supply is asked for, refilled per walk and never freed.
        CellRequest mAsking;
    };
}
