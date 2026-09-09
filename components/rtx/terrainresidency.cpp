#include "terrainresidency.hpp"

#include <chrono>
#include <mutex>

#include <components/loadinglistener/reporter.hpp>
#include <components/terrain/chunktaker.hpp>
#include <components/terrain/view.hpp>
#include <components/terrain/world.hpp>

#include "frameclock.hpp"

namespace Rtx
{
    TerrainResidency::TerrainResidency() = default;

    TerrainResidency::~TerrainResidency()
    {
        mYield = true;
        mWorker.request_stop();
        mWake.notify_all();
    }

    void TerrainResidency::follow(Terrain::World* terrain)
    {
        if (mTerrain == terrain)
            return;

        // **The thread holds the world it warms and the view it warms into**, and reads both
        // without the lock — which is sound only because it is stopped and joined here, before
        // either is replaced. Assigning over it requests its stop and joins it, and the yield is
        // what cuts short a preload already walking a quad tree that is about to go.
        mYield = true;
        mWorker = {};

        mTerrain = terrain;
        mView = terrain == nullptr ? nullptr : terrain->createView();
        mWarmView = terrain == nullptr ? nullptr : terrain->createView();
        mAskedOnce = false;

        mYield = false;
        if (mView != nullptr && mWarmView != nullptr)
            mWorker = std::jthread([this](std::stop_token stop) { warm(stop); });
    }

    void TerrainResidency::collect(Collector& into)
    {
        mWarmedMs = 0.0;

        if (mTerrain == nullptr || mView == nullptr)
            return;

        // **The mask, which the graph would have applied and this route goes around.** A walk that
        // may not see the terrain root may not see the chunks hanging off nothing below it either.
        if (!into.wouldReach(mTerrain->getTerrainRoot()))
            return;

        // **Before the collect and not after it.** What this asks for is where the eye is going, and
        // the thread has until the next frame to build it; asking afterwards would spend the frame
        // this one still has.
        ask();

        // **The frame builds chunks alone, and the thread stands out of the way for it.** Both reach
        // `QuadTreeWorld::loadRenderingNode`, whose caches neither lock nor rebuild atomically —
        // `mBuilding` says what that costs the picture. The yield is what bounds the wait to the one
        // chunk the thread is inside, so a frame pays a chunk and never a square.
        //
        // **Timed, because how long that bound actually is decides what the fold can be moved to.**
        // A chunk build is about a millisecond and the thread is asleep wherever the eye stands
        // still, so the wait is nothing most of the time and a chunk when the eye moves — but which
        // it is, and how often, is the question `getWarmedMs` is the answer to.
        const std::chrono::steady_clock::time_point asked = std::chrono::steady_clock::now();

        mYield = true;
        std::lock_guard<std::mutex> building(mBuilding);
        mYield = false;

        mWarmedMs = since(asked, std::chrono::steady_clock::now());

        mTerrain->collect(mView.get(),
            Terrain::Vantage{
                .mViewPoint = mViewPoint,
                .mGrid = mTerrain->getActiveGrid(),
                .mEnabled = mTerrain->isEnabled(),
            },
            into);
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

            {
                std::lock_guard<std::mutex> building(mBuilding);

                // **Nothing reports anywhere.** The reporter is what a loading screen counts chunks
                // with, and this warms behind a frame that is already being drawn.
                Loading::Reporter counted;

                mWarmView->reset();
                mTerrain->preload(mWarmView.get(), point, grid, mYield, counted);
            }

            lock.lock();
        }
    }
}
