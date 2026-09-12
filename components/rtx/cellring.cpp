#include "cellring.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "fogbuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// How many cells past the reach are prepared before they can be seen: one, which at the
        /// island route's speed is most of a second, so the frame a cell crosses into the reach owes
        /// only its placements.
        constexpr int sPreparedBand = 1;

        /// The order the prepared ring's missing cells are read in: nearest first, then a fixed
        /// order among equals, so two runs from one eye ask for one list.
        struct Nearer
        {
            osg::Vec2i mEye;

            bool operator()(const osg::Vec2i& left, const osg::Vec2i& right) const
            {
                const int leftAway = std::max(std::abs(left.x() - mEye.x()), std::abs(left.y() - mEye.y()));
                const int rightAway = std::max(std::abs(right.x() - mEye.x()), std::abs(right.y() - mEye.y()));
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

    int CellRing::reachInCells() const
    {
        return static_cast<int>(std::ceil(mAround.mReach / sCellSize));
    }

    void CellRing::giveBackHolds(
        const std::span<PreparedModel* const> models, const std::span<PreparedTexture* const> textures)
    {
        CellReturns& back = mSupply.giveBack();
        back.mModels.insert(back.mModels.end(), models.begin(), models.end());
        back.mTextures.insert(back.mTextures.end(), textures.begin(), textures.end());
    }

    void CellRing::takeDone()
    {
        mDoneScratch.clear();
        mSupply.take(mDoneScratch);

        for (PreparedCell* cell : mDoneScratch)
        {
            // Counted as it arrives, so the frame knows of every model a cell it may adopt names.
            for (PreparedModel* model : cell->mModels)
                ++mHolds.know(*model).mHanded;

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

    void CellRing::ask(const osg::Vec2i& eye, const int band)
    {
        // Rebuilt only when what it depends on moved: the eye's cell, what is held, what is handed,
        // or the statics switch. Otherwise it is the list the supply already has.
        if (!mAskStale)
            return;
        mAskStale = false;

        mAsking.mCells.clear();
        mAsking.mStatics = mStatics;

        for (int x = eye.x() - band; x <= eye.x() + band; ++x)
            for (int y = eye.y() - band; y <= eye.y() + band; ++y)
            {
                const osg::Vec2i cell(x, y);
                if (!holds(cell) && !handed(cell))
                    mAsking.mCells.push_back(cell);
            }

        std::sort(mAsking.mCells.begin(), mAsking.mCells.end(), Nearer{ eye });

        mSupply.ask(mAsking);
    }

    void CellRing::sift(const osg::Vec2i& eye, const int band)
    {
        const std::size_t before = mHanded.size();
        std::erase_if(mHanded, [&](PreparedCell* cell) {
            if (withinCells(cell->mCell, eye, band) && !holds(cell->mCell))
                return false;

            discard(*cell);
            return true;
        });
        mAskStale = mAskStale || mHanded.size() != before;
    }

    void CellRing::waitForNext(const osg::Vec2i& eye, const int band)
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
        HeldCell held = mSpareCells.take();
        held.mDropped = false;
        held.mCell = cell.mCell;
        held.mStatics = cell.mStatics;
        held.mModels.clear();

        // Emptied and kept, not reset, so the texture list a spare cell grew is room the next
        // one refills rather than a heap call on the frame a cell lands.
        if (held.mGround.has_value())
            held.mGround->reuse();

        mPlacer.adoptGround(cell, held, mHolds, mAround, stats);

        for (PreparedModel* model : cell.mModels)
        {
            CellHolds::HeldModel& known = mHolds.knownOf(*model);
            if (known.mParts.empty())
                mHolds.adoptParts(known, into);

            ++known.mHeld;
            --known.mHanded;
            held.mModels.push_back(model);
        }

        mPlacer.adoptPlacements(cell, held, mHolds);

        mCells.insert(std::move(held));

        mSupply.giveBack().mCells.push_back(&cell);
    }

    void CellRing::discard(PreparedCell& cell)
    {
        for (PreparedModel* model : cell.mModels)
            mHolds.release(*model, false);

        // Every hold the reader counted for the cell goes back with it: the models, and the
        // images its ground names.
        CellReturns& back = mSupply.giveBack();
        for (const PreparedLayer& layer : cell.mGround.mLayers)
            back.mTextures.push_back(layer.mTexture);
        giveBackHolds(cell.mModels, {});

        back.mCells.push_back(&cell);
    }

    void CellRing::dropCell(HeldCell& cell)
    {
        mPlacer.dropSlots(cell);

        for (PreparedModel* model : cell.mModels)
            mHolds.release(*model, true);

        const std::span<PreparedTexture* const> textures = cell.mGround.has_value()
            ? std::span<PreparedTexture* const>(cell.mGround->mTextures)
            : std::span<PreparedTexture* const>();
        giveBackHolds(cell.mModels, textures);

        mPlacer.dropGround(cell, mHolds);

        cell.mPlacements.clear();
        cell.mModels.clear();
        mSpareCells.give(std::move(cell));
    }

    void CellRing::setReferenceEnabled(const ESM::RefNum refnum, const bool enabled)
    {
        mPlacer.setReferenceEnabled(refnum, enabled, std::span<HeldCell>(mCells.begin(), mCells.end()));
    }

    void CellRing::dropPlacements()
    {
        for (HeldCell& cell : mCells)
            mPlacer.dropSlots(cell);
    }

    void CellRing::collect(SceneAdopter& into, ExtractionStats& stats)
    {
        // What `forget` let go of since the last walk, and then what this walk lets go of.
        mHolds.releaseParts(into);
        walkRings(into, stats);
        mHolds.releaseParts(into);

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
        if (!mAround.mOutdoors)
        {
            dropPlacements();
            mSupply.publish();
            return;
        }

        const osg::Vec2i eye = cellOf(mAround.mEye);
        const int reach = reachInCells();
        const int band = reach + sPreparedBand;

        if (mLastEye != eye)
        {
            mLastEye = eye;
            mAskStale = true;
        }

        // A cell held with the statics the other way is dropped whole and read again, for the
        // reason `takeDone` gives.
        // Dropped in place and compacted once: an erase per cell shifts the tail per cell, and a
        // worldspace change drops many in one frame.
        bool dropped = false;
        for (HeldCell& cell : mCells)
        {
            if (withinCells(cell.mCell, eye, band) && cell.mStatics == mStatics)
                continue;

            dropCell(cell);
            cell.mDropped = true;
            dropped = true;
        }
        if (dropped)
        {
            mCells.eraseIf([](const HeldCell& cell) { return cell.mDropped; });
            mAskStale = true;
        }

        sift(eye, band);

        ask(eye, band);

        // Waited for after the ask that names it and never before, because what the reader is
        // about to hand back is what that ask asked for. Nothing was asked for where the band is
        // whole, and then there is nothing to wait for.
        if (mSettled && mHanded.empty() && !mAsking.mCells.empty())
            waitForNext(eye, band);

        adoptHanded(into, stats);

        for (HeldCell& cell : mCells)
            mPlacer.place(cell, mAround, eye, reach);

        mSupply.publish();
    }
}
