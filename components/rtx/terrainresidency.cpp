#include "terrainresidency.hpp"

#include <components/loadinglistener/reporter.hpp>
#include <components/terrain/view.hpp>
#include <components/terrain/world.hpp>

namespace Rtx
{
    TerrainResidency::TerrainResidency() = default;

    TerrainResidency::~TerrainResidency()
    {
        mAbort = true;
        mWorker.request_stop();
        mWake.notify_all();
    }

    void TerrainResidency::follow(Terrain::World* terrain)
    {
        if (mTerrain == terrain)
            return;

        // **The thread holds the world it warms and the view it warms into**, and reads both
        // without the lock — which is sound only because it is stopped and joined here, before
        // either is replaced. Assigning over it requests its stop and joins it, and the abort is
        // what cuts short a preload already walking a quad tree that is about to go.
        mAbort = true;
        mWorker = {};

        mTerrain = terrain;
        mView = terrain == nullptr ? nullptr : terrain->createView();
        mWarmView = terrain == nullptr ? nullptr : terrain->createView();
        mAskedOnce = false;

        mAbort = false;
        if (mView != nullptr && mWarmView != nullptr)
            mWorker = std::jthread([this](std::stop_token stop) { warm(stop); });
    }

    void TerrainResidency::collect(osg::NodeVisitor& visitor)
    {
        if (mTerrain == nullptr || mView == nullptr)
            return;

        // **Before the collect and not after it.** What this asks for is where the eye is going, and
        // the thread has until the next frame to build it; asking afterwards would spend the frame
        // this one still has.
        ask();

        mTerrain->collect(mView.get(), mViewPoint, visitor);
    }

    void TerrainResidency::ask()
    {
        if (mWarmView == nullptr)
            return;

        // The square `collect` below will resolve against. Warming a different one would build
        // chunks at levels of detail nothing is about to ask for.
        const osg::Vec4i grid = mTerrain->getActiveGrid();

        // **An eye that has not moved has nothing new to warm.** Asking anyway would have the thread
        // walk the whole quad tree again for the answer it just gave, over and over for as long as
        // somebody stands still — a core burnt for nothing. Exactly equal rather than near enough,
        // because a still eye reports the same point and a moving one never does.
        if (mAskedOnce && mViewPoint == mLastAsked && grid == mLastGrid)
            return;

        osg::Vec3f lead;
        if (mAskedOnce)
        {
            lead = (mViewPoint - mLastAsked) * sLeadSteps;
            if (const float reach = lead.length(); reach > sLeadLimit)
                lead *= sLeadLimit / reach;
        }

        mLastAsked = mViewPoint;
        mLastGrid = grid;
        mAskedOnce = true;

        {
            std::lock_guard<std::mutex> lock(mMutex);
            mWantedPoint = mViewPoint + lead;
            mWantedGrid = grid;
            mWanted = true;
        }

        mWake.notify_one();
    }

    void TerrainResidency::warm(std::stop_token stop)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        while (mWake.wait(lock, stop, [&] { return mWanted; }))
        {
            if (stop.stop_requested())
                return;

            mWanted = false;
            const osg::Vec3f point = mWantedPoint;
            const osg::Vec4i grid = mWantedGrid;
            lock.unlock();

            // **Nothing reports anywhere.** The reporter is what a loading screen counts chunks
            // with, and this warms behind a frame that is already being drawn.
            Loading::Reporter counted;
            mWarmView->reset();
            mTerrain->preload(mWarmView.get(), point, grid, mAbort, counted);

            lock.lock();
        }
    }
}
