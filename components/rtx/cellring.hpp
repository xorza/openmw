#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

#include <osg/Image>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4i>

#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>

#include "index.hpp"
#include "material.hpp"
#include "mirroridentity.hpp"
#include "preparedcell.hpp"
#include "preparedtexture.hpp"
#include "sceneextractor.hpp"
#include "texturebuilder.hpp"

namespace Terrain
{
    class ObjectStorage;
    class Storage;
}

namespace Rtx
{
    class CellReader;
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
        CellRing(SceneExtractor& extractor, SceneDesc& scene);

        /// Stops the thread. A cell in flight is finished and dropped rather than waited out.
        ~CellRing() override;

        CellRing(const CellRing&) = delete;
        CellRing& operator=(const CellRing&) = delete;

        /// What to read and where content comes from. Null for a world with none, which `collect`
        /// answers by standing nothing; a change of any, or of worldspace, drops everything held
        /// and starts again.
        ///
        /// @param mask which nodes a walk of a template may descend into — the frame walk's own.
        void follow(const Terrain::ObjectStorage* storage, Terrain::Storage* ground, ContentSource* content,
            ESM::RefId worldspace, osg::Node::NodeMask mask);

        /// Whether the ring stands the distance's statics at all. The ground stands either way;
        /// off is the A/B `--distant-statics=false` is, and a change of it reads every cell again.
        void setStaticsEnabled(bool enabled);

        /// How far out cells are placed, in units — `distantLandReach`, which is what the air is
        /// built to as well. Told rather than asked, because this library reads no settings.
        void setReach(float units);

        /// The size rule's constant: a reference is placed while its scaled radius is at least this
        /// times the eye's distance to its cell. `object paging min size`, told for the same reason
        /// the reach is.
        void setMinSize(float minSize);

        /// The cells the game has stood for itself, as `Terrain::World` states them: minimum
        /// inclusive, maximum exclusive. No static is placed inside it, and the ground inside it
        /// shades from its layer stack rather than from a composite.
        void setActiveGrid(const osg::Vec4i& grid);

        /// Where the eye is, which decides both rings and the size rule.
        void setViewPoint(const osg::Vec3f& viewPoint);

        /// Whether there is a distant world to stand in. False in an interior, where the eye's
        /// coordinates belong to another space: what is held stays held and nothing is placed.
        void setOutdoors(bool outdoors);

        /// The frame the next walk is for, so a frame walked twice adopts one cell and not two.
        void setFrame(std::size_t frame);

        /// Whether a walk waits for every cell of the prepared ring before it places any.
        ///
        /// **Which frame a cell is adopted on is otherwise the thread's answer**, and a run whose
        /// pictures are compared with another's cannot have that. `CompositeQueue::setSettled` is
        /// the same rule for the ground's composites.
        void setSettled(bool settled);

        /// What the game says of one reference, which the content files cannot: a script has
        /// disabled it, or enabled it again. Placed or dropped on the next walk.
        void setReferenceEnabled(ESM::RefNum refnum, bool enabled);

        void collect(Collector& into) override;

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

            /// The ground's rows, or none where the cell stands no ground.
            Index mGroundMesh = sNoIndex;
            Index mGroundMaterial = sNoIndex;
            Index mGroundSlot = sNoIndex;
            osg::Vec3f mGroundOrigin;

            /// How many layers the ground's stack holds, which is whether a composite is worth
            /// asking for at all.
            std::uint32_t mLayers = 0;

            /// Whether the material row asks for a composite now, so a crossing of the active
            /// grid rewrites it once.
            bool mFlattened = false;

            /// The readings of the ground's textures, held for as long as the cell is.
            std::vector<PreparedTexture*> mGroundTextures;
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

        /// How far the eye stands from the nearest point of `cell`, in units, by the square's own
        /// metric — which is the one the paging measured a chunk's reach by.
        static float distanceTo(const osg::Vec2i& cell, const osg::Vec3f& eye);

        static bool withinBand(const osg::Vec2i& cell, const osg::Vec2i& eye, int band);

        bool inActiveGrid(const osg::Vec2i& cell) const;
        bool holds(const osg::Vec2i& cell) const;
        bool pending(const osg::Vec2i& cell) const;
        bool isDisabled(ESM::RefNum refnum) const;
        int reachInCells() const;

        /// Whether a held cell's ground wants its stack flattened where the eye stands now.
        bool wantsFlattening(const HeldCell& cell) const;

        /// Counts one more holder of `texture`'s reading, for `find`.
        void holdTexture(const PreparedTexture& texture);

        /// Counts one holder off, and forgets the reading where none is left.
        void dropTexture(const PreparedTexture& texture);

        /// Moves the thread's finished cells into the frame's own list, counting their models. A
        /// cell read with the statics the other way is let go of here.
        void takeDone();

        /// Hands the thread the cells the prepared ring lacks, nearest first.
        void ask(const osg::Vec2i& eye, int band);

        /// Blocks until every cell the prepared ring lacks has been read. See `setSettled`.
        void waitForWanted(const osg::Vec2i& eye, int band);

        /// Adopts what the thread read: one cell, or every cell where the run is settled.
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

        /// Hands the reader a cell's holds on its models and its ground's textures, for a cell the
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

        /// Hands the thread what the frame gave back this walk.
        void publishReturns();

        /// Gives the reader what the frame gave back. On the thread, under the lock.
        void recycle();

        /// Places and drops by the rings and the size rule, and flattens by the grid.
        void place(const osg::Vec2i& eye, int reach);

        /// Says the walk met every mesh and material the ring holds.
        void stamp();

        void work(std::stop_token stop);

        SceneExtractor& mExtractor;
        SceneDesc& mScene;

        const Terrain::ObjectStorage* mStorage = nullptr;
        Terrain::Storage* mGround = nullptr;
        ContentSource* mContent = nullptr;
        ESM::RefId mWorldspace;
        osg::Node::NodeMask mMask = ~0u;

        bool mStatics = true;
        float mReach = 0.0f;
        float mMinSize = 0.0f;
        osg::Vec4i mActiveGrid;
        osg::Vec3f mViewPoint;
        bool mOutdoors = true;
        bool mSettled = false;

        std::size_t mFrame = 0;

        /// The frame a cell was last adopted on, so a frame walked twice adopts once.
        std::size_t mAdoptedFrame = ~std::size_t{ 0 };

        /// Sorted by cell, so a walk over them is the same walk on every machine — which is what
        /// makes the slot a placement takes a fact about the world.
        std::vector<HeldCell> mCells;
        std::vector<HeldCell> mSpareCells;

        /// Sorted by the model's address, and kept when an entry goes, for the reason the cells are.
        std::vector<HeldModel> mModels;
        std::vector<HeldModel> mSpareModels;

        /// Sorted by the image's address.
        std::vector<HeldTexture> mTextures;

        /// What the thread read and the frame has not adopted yet, in the order it arrived.
        std::vector<PreparedCell*> mPending;

        /// References a script has disabled, sorted.
        std::vector<ESM::RefNum> mDisabled;

        std::uint32_t mPlaced = 0;
        std::uint32_t mGroundPlaced = 0;

        // Refilled per frame.
        std::vector<osg::Vec2i> mMissingScratch;
        std::vector<PreparedCell*> mDoneScratch;
        std::vector<PreparedCell*> mReturnCellsScratch;
        std::vector<PreparedModel*> mReturnModelsScratch;
        std::vector<PreparedTexture*> mReturnTexturesScratch;
        std::vector<MaterialLayer> mLayerScratch;

        /// The last list handed to the thread and what it was to read of each cell, so an unchanged
        /// ask is not handed over again.
        std::vector<osg::Vec2i> mRequested;
        bool mRequestedStatics = true;

        std::mutex mMutex;
        std::condition_variable_any mWake;
        std::condition_variable_any mDoneWake;

        /// What the thread is to read next, written whole under the lock and taken whole by the
        /// thread. A newer list replaces an older one it has not finished.
        std::vector<osg::Vec2i> mWanted;
        bool mWantedStatics = true;

        /// What the thread has read, under the lock.
        std::vector<PreparedCell*> mDone;

        /// What the frame has given back, under the lock, for the thread to refill.
        std::vector<PreparedCell*> mReturnedCells;
        std::vector<PreparedModel*> mReturnedModels;
        std::vector<PreparedTexture*> mReturnedTextures;

        /// The thread's own: the list it is working through, and whether that list wants statics.
        std::vector<osg::Vec2i> mRequest;
        bool mRequestStatics = true;

        /// Owned here and used by the thread alone while it runs.
        std::unique_ptr<CellReader> mReader;

        /// **Last, so it is joined before anything it touches is destroyed.**
        std::jthread mWorker;
    };
}
