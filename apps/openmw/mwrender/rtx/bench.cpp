#include "bench.hpp"

#ifdef OPENMW_RTX_BENCH

#include <cmath>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>

#include <osg/Vec3f>

#include <components/debug/debuglog.hpp>
#include <components/esm/position.hpp>
#include <components/rtx/frametimes.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchspec.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/refdata.hpp"

namespace MWRender
{
    /// What the run accumulates. Out of line so the header names no container.
    struct Bench::Held
    {
        Rtx::FrameSamples mSamples;
        Rtx::GpuBreakdown mGpu;
    };

    Bench::Bench()
    {
        const char* const spec = std::getenv("OPENMW_RTX_BENCH");
        if (spec == nullptr || *spec == '\0')
            return;

        // **The one parser both hosts read**, so a run asked for here and the same run asked for
        // through `openmw-rtxtool bench` are one run rather than two spellings of one.
        std::string complaint;
        const std::optional<Rtx::BenchSpec> read = Rtx::readSpec(spec, complaint);

        // **A run nobody can read the settings of is not a run.** A spec that will not parse is a
        // typo in a benchmark somebody is about to trust, and starting anyway would hand them a
        // number for a length they did not ask for.
        if (!read.has_value())
        {
            Log(Debug::Error) << "OPENMW_RTX_BENCH: " << complaint;
            return;
        }

        mSpec = *read;
        mHeld = std::make_unique<Held>();

        // Reserved once at a rate no frame will beat, so the run never grows a vector — a benchmark
        // that stops to reallocate is measuring its own allocator.
        mHeld->mSamples.reserve(mSpec.getMeasured());

        Log(Debug::Info) << "Ray tracing bench: " << mSpec.mRun.describe() << " after " << mSpec.mWarm.describe()
                         << " warming up";
    }

    Bench::~Bench() = default;

    void Bench::frame(const Rtx::FrameResult& result, double frameMs, double walkMs, double placeMs, bool rebuilt)
    {
        if (mHeld == nullptr || mDone)
            return;

        if (mSeen < mSpec.getWarmup())
        {
            ++mSeen;
            return;
        }

        mHeld->mSamples.add(frameMs, walkMs, placeMs);
        mHeld->mSamples.addWait(result.mWaitMs);
        mHeld->mGpu.add(result.mGpu);
        mMeasuredMs += frameMs;

        fly(frameMs, rebuilt);

        if (mHeld->mSamples.size() < mSpec.getMeasured())
            return;

        report();
        mDone = true;

        // **The way the quit key ends a session, and not `exit`.** A run that tore the process down
        // where it stood would leave the save, the log and the device wherever they happened to be,
        // and the next thing anyone would debug is the benchmark.
        MWBase::Environment::get().getStateManager()->requestQuit();
    }

    void Bench::fly(double frameMs, bool rebuilt)
    {
        if (!(mSpec.mSpeed > 0.0f))
            return;

        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world.getPlayerPtr();
        if (player.isEmpty())
            return;

        // **Counted before the move below and not after it.** The move pulls the next ring in, and
        // that read lands in the frame after this one — which is the frame that arrives here
        // standing in a cell it was not drawn in last time, and the frame that paid for it.
        //
        // **The whole frame goes in as the read**, because the game gives this no split: the ring
        // arrives on the loading threads and what a crossing costs here is the frame that dropped.
        const void* cell = player.getCell();
        if (mCell != nullptr && cell != mCell)
            mCrossings.add(rebuilt, frameMs, 0.0);

        mCell = cell;

        // **The heading the engine measures**, which is clockwise from north rather than
        // counter-clockwise from east, and horizontal: a route follows the ground the cells are laid
        // out on, and the pitch a save happens to have left would fly it into the sky or the sea.
        const ESM::Position& stood = player.getRefData().getPosition();
        const osg::Vec3f ahead(std::sin(stood.rot[2]), std::cos(stood.rot[2]), 0.0f);

        // **Held at the height the save left**, because nothing here flies: gravity would sink the
        // route into the sea over ten seconds, and a route that ends underwater measures the wrong
        // frame. The ground still rises through it, so a stretch of a long route is inside a hill.
        if (!mHeight.has_value())
            mHeight = stood.pos[2];

        osg::Vec3f step = ahead * (mSpec.mSpeed * static_cast<float>(frameMs) / 1000.0f);
        step.z() = *mHeight - stood.pos[2];

        // **`moveObjectBy` and not `moveObject`, because the player is an actor.** The actor's
        // position lives in the physics world as well, and a move that writes only the world's copy
        // is written back over it on the next step, so the route never leaves the cell it began in.
        world.moveObjectBy(player, step, true);
    }

    void Bench::report()
    {
        Rtx::FrameSamples& samples = mHeld->mSamples;

        // **No view, no cell and no scene, so those lines are not printed.** What the game knows
        // about a run is how long its frames took and what it crossed; where it stood is the
        // savegame's answer and belongs in whatever asked for the run.
        //
        // Assigned rather than named in an initialiser, because this file is compiled with
        // upstream's warnings and a designated initializer that leaves a field out is one of them.
        Rtx::BenchPlace place;
        place.mFrames = samples.size();
        place.mWallSeconds = mMeasuredMs / 1000.0;
        place.mFrame = Rtx::summarise(samples.mFrame);
        place.mWait = Rtx::summarise(samples.mWait);
        place.mWalk = Rtx::summarise(samples.mWalk);
        place.mPlace = Rtx::summarise(samples.mPlace);
        place.mCrossings = mCrossings;

        const std::span<const Rtx::GpuZone> zones = mHeld->mGpu.summariseZones();
        place.mGpu.assign(zones.begin(), zones.end());

        // Built whole and logged once: the report is a table, and a table split across log lines by
        // a timestamp apiece is not one.
        Log(Debug::Info) << "\nRay tracing bench" << Rtx::describePlace(place);
    }
}

#endif
