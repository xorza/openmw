#include "cellsupply.hpp"

#include <utility>

#include "cellreader.hpp"
#include "prepared.hpp"

namespace Rtx
{
    void CellRequest::take(CellRequest& from)
    {
        // Swapped rather than copied: what this hands back is the room the last request grew, which
        // the next one to reach the frame's side refills.
        mCells.swap(from.mCells);
        mStatics = from.mStatics;

        from.clear();
    }

    void CellReturns::clear()
    {
        mCells.clear();
        mModels.clear();
        mTextures.clear();
    }

    void CellReturns::take(CellReturns& from)
    {
        mCells.insert(mCells.end(), from.mCells.begin(), from.mCells.end());
        mModels.insert(mModels.end(), from.mModels.begin(), from.mModels.end());
        mTextures.insert(mTextures.end(), from.mTextures.begin(), from.mTextures.end());

        from.clear();
    }

    CellSupply::CellSupply() = default;

    CellSupply::~CellSupply() = default;

    void CellSupply::follow(const CellWorld& world)
    {
        mOnFrame.check();

        // The thread reads the storages and the content without the lock, which is sound only
        // because it is stopped and joined here before any is replaced — and before the reader that
        // holds them goes. Nothing is given back: what the frame held dies with the reader.
        mWorker.stop();

        mWanted.clear();
        mRequested.clear();
        mReading.clear();
        mDone.clear();
        mReturned.clear();
        mReturning.clear();
        mReader.reset();

        mWorld = world;

        if (!mWorld.isReadable())
            return;

        mReader = std::make_unique<CellReader>(
            *mWorld.mStorage, *mWorld.mGround, *mWorld.mContent, mWorld.mWorldspace, mWorld.mMask);
        mWorker.start([this](std::stop_token stop) { work(stop); });
    }

    void CellSupply::ask(const CellRequest& request)
    {
        mOnFrame.check();

        if (mReader == nullptr || request == mRequested)
            return;

        mRequested = request;

        mMonitor.give([&] { mWanted = request; });
    }

    void CellSupply::take(std::vector<PreparedCell*>& into)
    {
        // Asked here, because this is the one call every frame makes. A reader that threw is a
        // world that cannot be read, and the frame learns it where it would have taken a cell.
        mMonitor.rethrowFailure();

        mMonitor.under([&] {
            into.insert(into.end(), mDone.begin(), mDone.end());
            mDone.clear();
        });
    }

    bool CellSupply::waitForOne()
    {
        return mMonitor.await([&] { return !mDone.empty(); });
    }

    void CellSupply::publish()
    {
        mOnFrame.check();

        if (mReturning.empty())
            return;

        mMonitor.give([&] { mReturned.take(mReturning); });
    }

    void CellSupply::recycle()
    {
        for (PreparedCell* cell : mReturned.mCells)
            mReader->giveBack(*cell);
        mReturned.mCells.clear();

        for (PreparedTexture* texture : mReturned.mTextures)
            mReader->giveBack(*texture);
        mReturned.mTextures.clear();

        for (PreparedModel* model : mReturned.mModels)
            mReader->giveBack(*model);
        mReturned.mModels.clear();
    }

    void CellSupply::work(std::stop_token stop)
    {
        mMonitor.serve(
            stop, [&] { return !mWanted.empty() || !mReturned.empty(); },
            [&] {
                recycle();
                mReading.take(mWanted);
            },
            [&](std::stop_token turn) { read(turn); });
    }

    void CellSupply::read(std::stop_token stop)
    {
        for (const osg::Vec2i& cell : mReading.mCells)
        {
            if (stop.stop_requested())
                return;

            // A newer list replaces this one: the eye has moved and what it lacks has changed.
            const bool newer = mMonitor.under([&] {
                recycle();
                return !mWanted.empty();
            });

            if (newer)
                return;

            PreparedCell& made = mReader->read(cell, mReading.mStatics);

            mMonitor.hand([&] { mDone.push_back(&made); });
        }
    }
}
