#include "cellring.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "cellgrid.hpp"
#include "extractionstats.hpp"

namespace Rtx
{
    namespace
    {
        /// How far past the reach a cell is prepared before it can be seen: one cell, which at the
        /// island route's speed is most of a second, so the frame a cell crosses into the reach owes
        /// only its placements.
        constexpr float sPreparedBand = sCellSize;

        /// The order the prepared disc's missing cells are read in: nearest first, then a fixed
        /// order among equals, so two runs from one eye ask for one list.
        struct Nearer
        {
            osg::Vec3f mEye;

            bool operator()(const osg::Vec2i& left, const osg::Vec2i& right) const
            {
                const float leftAway = distanceSquaredTo(left, mEye);
                const float rightAway = distanceSquaredTo(right, mEye);
                if (leftAway != rightAway)
                    return leftAway < rightAway;

                return left < right;
            }
        };
    }

    CellRing::CellRing(SceneDesc& scene)
        : mPlacer(scene)
    {
    }

    CellRing::~CellRing() = default;

    void CellRing::follow(const WorldAround& around)
    {
        mAround = around;

        if (mSupply.isReading(around.mWorld))
            return;

        // Everything held names the reader that is about to go, so it is let go of before the
        // supply is pointed anywhere else. Nothing is given back: what the frame held dies with the
        // reader that lent it.
        forget();
        mSupply.follow(around.mWorld);
        mAskStale = true;
    }

    void CellRing::forget()
    {
        for (HeldCell& cell : mCells)
        {
            mPlacer.dropSlots(cell);
            mPlacer.dropGround(cell, mHolds);
        }

        mHolds.forget();
        mCells.clear();
        mHanded.clear();
    }

    void CellRing::setStaticsEnabled(const bool enabled)
    {
        mAskStale = mAskStale || enabled != mStatics;
        mStatics = enabled;
    }

    void CellRing::setFrame(const std::size_t frame)
    {
        mFrame = frame;
    }

    void CellRing::setSettled(const bool settled)
    {
        mSettled = settled;
    }

    bool CellRing::holds(const osg::Vec2i& cell) const
    {
        return mCells.contains(cell);
    }

    bool CellRing::handed(const osg::Vec2i& cell) const
    {
        return std::any_of(
            mHanded.begin(), mHanded.end(), [&](const PreparedCell* held) { return held->mCell == cell; });
    }

    void CellRing::giveBackHolds(const HeldCell& cell)
    {
        CellReturns& back = mSupply.giveBack();
        back.mModels.insert(back.mModels.end(), cell.mModels.begin(), cell.mModels.end());
        if (cell.mGround.has_value())
            back.mTextures.insert(back.mTextures.end(), cell.mGround->mTextures.begin(), cell.mGround->mTextures.end());
    }

    void CellRing::giveBackHolds(const PreparedCell& cell)
    {
        CellReturns& back = mSupply.giveBack();
        back.mModels.insert(back.mModels.end(), cell.mModels.begin(), cell.mModels.end());
        for (const PreparedLayer& layer : cell.mGround.mLayers)
            back.mTextures.push_back(layer.mTexture);
    }

    void CellRing::takeDone()
    {
        mDoneScratch.clear();
        mSupply.take(mDoneScratch);

        for (PreparedCell* cell : mDoneScratch)
        {
            // Counted as it arrives, so the frame knows of every model a cell it may adopt names.
            for (PreparedModel* model : cell->mModels)
                ++mHolds.know(*model).mNamed;

            // The switch is a setting the game can move while it runs, and a cell read under the
            // other answer is read again. A cell the reader is part-way through is neither held nor
            // handed, so a replacing list names it again and the reader hands over two copies;
            // `sift` turns the second away.
            if (cell->mStatics != mStatics || handed(cell->mCell))
                discard(*cell);
            else
                mHanded.push_back(cell);
            mAskStale = true;
        }

        mDoneScratch.clear();
    }

    void CellRing::ask(const osg::Vec3f& eye, const float band)
    {
        // Rebuilt only when what it depends on moved: the eye, what is held, what is handed, or the
        // statics switch. Otherwise it is the list the supply already has.
        if (!mAskStale)
            return;
        mAskStale = false;

        mAsking.mCells.clear();
        mAsking.mStatics = mStatics;

        forEachCellWithin(eye, band, [&](const osg::Vec2i& cell) {
            if (!holds(cell) && !handed(cell))
                mAsking.mCells.push_back(cell);
        });

        std::sort(mAsking.mCells.begin(), mAsking.mCells.end(), Nearer{ eye });

        mSupply.ask(mAsking);
    }

    void CellRing::sift(const osg::Vec3f& eye, const float band)
    {
        const std::size_t before = mHanded.size();
        std::erase_if(mHanded, [&](PreparedCell* cell) {
            if (withinReach(cell->mCell, eye, band) && !holds(cell->mCell))
                return false;

            discard(*cell);
            return true;
        });
        mAskStale = mAskStale || mHanded.size() != before;
    }

    void CellRing::waitForNext(const osg::Vec3f& eye, const float band)
    {
        // A cell read under the other answer to the statics switch, or one of a band that left, is
        // not what the wait waited for: the first thing the reader hands over after a move is
        // usually a cell nothing wants any more.
        while (mHanded.empty())
        {
            // Given up on where the reader has gone, which is a reader that threw. Waiting on
            // one that can hand nothing over is a wait with no end, and a wait that answered at
            // once with no cell would be a spin instead. The walk adopts nothing this frame and
            // `CellSupply::take` is where the failure is reported.
            if (!mSupply.waitForOne())
                return;

            takeDone();
            sift(eye, band);
        }
    }

    void CellRing::adoptHanded(SceneAdopter& into, ExtractionStats& stats)
    {
        // One cell a frame, and one frame walked twice adopts once. A cell's meshes are copied
        // into the scene and its structures built by the hand-over that follows; two on one frame
        // would be the batch behind a threshold this renderer never takes. A settled walk keeps the
        // rule and waits for its one cell, which is what `setSettled` says.
        if (mHanded.empty() || mAdoptedFrame == mFrame)
            return;

        mAdoptedFrame = mFrame;
        adopt(*mHanded.front(), into, stats);
        mHanded.erase(mHanded.begin());
        mAskStale = true;
    }

    void CellRing::adopt(PreparedCell& cell, SceneAdopter& into, ExtractionStats& stats)
    {
        // A spare comes back through `reuse`, so what it holds is room and nothing else.
        HeldCell held = mSpareCells.take();
        held.mCell = cell.mCell;
        held.mStatics = cell.mStatics;

        mPlacer.adoptGround(cell, held, mHolds, mAround, stats);

        for (PreparedModel* model : cell.mModels)
        {
            CellHolds::HeldModel& known = mHolds.knownOf(*model);
            if (known.mParts.empty())
                mHolds.adoptParts(known, into);

            held.mModels.push_back(model);
        }

        mPlacer.adoptPlacements(cell, held, mHolds);

        mCells.insert(std::move(held));

        mSupply.giveBack().mCells.push_back(&cell);
    }

    void CellRing::discard(PreparedCell& cell)
    {
        for (PreparedModel* model : cell.mModels)
            mHolds.release(*model);

        // Every hold the reader counted for the cell goes back with it: the models, and the
        // images its ground names.
        giveBackHolds(cell);
        mSupply.giveBack().mCells.push_back(&cell);
    }

    void CellRing::dropCell(HeldCell& cell)
    {
        mPlacer.dropSlots(cell);

        for (PreparedModel* model : cell.mModels)
            mHolds.release(*model);

        giveBackHolds(cell);

        mPlacer.dropGround(cell, mHolds);

        cell.reuse();
        mSpareCells.give(std::move(cell));
    }

    void CellRing::setReferenceEnabled(const ESM::RefNum refnum, const bool enabled)
    {
        mPlacer.setReferenceEnabled(refnum, enabled, std::span<HeldCell>(mCells.begin(), mCells.end()));
    }

    void CellRing::forgetReferences()
    {
        mPlacer.forgetReferences(std::span<HeldCell>(mCells.begin(), mCells.end()));
    }

    void CellRing::dropPlacements()
    {
        for (HeldCell& cell : mCells)
            mPlacer.dropSlots(cell);
    }

    void CellRing::collectStanding(std::vector<ESM::RefNum>& into) const
    {
        for (const HeldCell& cell : mCells)
            for (std::size_t at = 0; at < cell.mShown; ++at)
                if (cell.mPlacements[at].mStood.isStanding())
                    into.push_back(cell.mPlacements[at].mRefNum);
    }

    bool CellRing::standsAsHeld() const
    {
        for (const HeldCell& cell : mCells)
            if (!mPlacer.standsAsHeld(cell, mAround))
                return false;

        return mPlacer.standsNoMore();
    }

    void CellRing::collect(SceneAdopter& into, ExtractionStats& stats)
    {
        // What `forget` let go of since the last walk, and then what this walk lets go of.
        mHolds.releaseParts(into);
        walkRings(into, stats);
        mHolds.releaseParts(into);

        assert(standsAsHeld() && "the ring stands something its cells do not hold, or holds what it does not stand");

        // What stands, counted off the slots and not off a tally: a world with no reader and an
        // interior have both dropped every slot by now, and stand nothing.
        stats.mInstances += mPlacer.getPlaced() + mPlacer.getGroundPlaced();
        stats.mDistantStatics += mPlacer.getPlaced();
        stats.mGroundCells += mPlacer.getGroundPlaced();
    }

    void CellRing::walkRings(SceneAdopter& into, ExtractionStats& stats)
    {
        if (!mSupply.hasReader())
            return;

        takeDone();

        // Indoors the eye's coordinates belong to another space, so the rings are not moved:
        // what is held stays held for the way back out, and nothing stands.
        if (!mAround.mExterior)
        {
            dropPlacements();
            mSupply.publish();
            return;
        }

        const osg::Vec3f& eye = mAround.mEye;
        const float band = mAround.mReach + sPreparedBand;

        // Any move, because the disc is measured from the eye itself and a cell at its rim can
        // enter or leave on a step. What that costs is a walk over the band's cells on the frames
        // the eye moves, and nothing on the frames it stands.
        if (mLastEye != eye)
        {
            mLastEye = eye;
            mAskStale = true;
        }

        // A cell held with the statics the other way is dropped whole and read again, for the
        // reason `takeDone` gives. Dropped in one walk and compacted once, because a worldspace
        // change drops many in one frame.
        const std::size_t dropped = mCells.dropIf([&](HeldCell& cell) {
            if (withinReach(cell.mCell, eye, band) && cell.mStatics == mStatics)
                return false;

            dropCell(cell);
            return true;
        });
        if (dropped > 0)
            mAskStale = true;

        sift(eye, band);

        ask(eye, band);

        // Waited for after the ask that names it and never before, because what the reader is
        // about to hand back is what that ask asked for. Nothing was asked for where the band is
        // whole, and then there is nothing to wait for.
        if (mSettled && mHanded.empty() && !mAsking.mCells.empty())
            waitForNext(eye, band);

        adoptHanded(into, stats);

        for (HeldCell& cell : mCells)
            stats.mLights += mPlacer.place(cell, mAround);

        mSupply.publish();
    }
}
