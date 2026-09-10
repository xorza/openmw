#include "cellsupply.hpp"

#include <utility>

#include "cellreader.hpp"
#include "preparedcell.hpp"
#include "preparedtexture.hpp"

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
        // **The thread reads the storages and the content without the lock**, which is sound only
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
        if (mReader == nullptr || request == mRequested)
            return;

        mRequested = request;

        {
            const std::lock_guard<std::mutex> lock(mMutex);
            mWanted = request;
        }

        mWake.notify_one();
    }

    void CellSupply::take(std::vector<PreparedCell*>& into)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        into.insert(into.end(), mDone.begin(), mDone.end());
        mDone.clear();
    }

    void CellSupply::waitForOne()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mDoneWake.wait(lock, [&] { return !mDone.empty(); });
    }

    void CellSupply::publish()
    {
        if (mReturning.empty())
            return;

        {
            const std::lock_guard<std::mutex> lock(mMutex);
            mReturned.take(mReturning);
        }

        mWake.notify_one();
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
        std::unique_lock<std::mutex> lock(mMutex);
        while (mWake.wait(lock, stop, [&] { return !mWanted.empty() || !mReturned.empty(); }))
        {
            if (stop.stop_requested())
                return;

            recycle();

            mReading.take(mWanted);
            lock.unlock();

            for (const osg::Vec2i& cell : mReading.mCells)
            {
                if (stop.stop_requested())
                    break;

                // A newer list replaces this one: the eye has moved and what it lacks has changed.
                lock.lock();
                const bool newer = !mWanted.empty();
                recycle();
                lock.unlock();

                if (newer)
                    break;

                PreparedCell& made = mReader->read(cell, mReading.mStatics);

                lock.lock();
                mDone.push_back(&made);
                lock.unlock();
                mDoneWake.notify_all();
            }

            lock.lock();
        }
    }
}
