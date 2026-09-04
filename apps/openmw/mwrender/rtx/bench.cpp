#include "bench.hpp"

#ifdef OPENMW_RTX_BENCH

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec3f>

#include <components/debug/debuglog.hpp>
#include <components/esm/position.hpp>
#include <components/rtx/frametimes.hpp>
#include <components/rtx/renderer.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/refdata.hpp"

namespace MWRender
{
    namespace
    {
        /// How much of a run a number asks for: frames, or seconds where it carries an `s`.
        struct Span
        {
            std::uint32_t mFrames = 0;
            double mSeconds = 0.0;

            bool empty() const { return mFrames == 0 && mSeconds <= 0.0; }
        };

        /// `240` frames, or `10s` seconds. Nothing where it is neither.
        std::optional<Span> readSpan(std::string_view text)
        {
            const bool timed = !text.empty() && text.back() == 's';
            if (timed)
                text.remove_suffix(1);

            std::uint32_t value = 0;
            const auto* end = text.data() + text.size();
            const std::from_chars_result read = std::from_chars(text.data(), end, value);
            if (read.ec != std::errc{} || read.ptr != end)
                return std::nullopt;

            return timed ? Span{ .mSeconds = static_cast<double>(value) } : Span{ .mFrames = value };
        }
    }

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

        std::string_view text(spec);

        // **The speed comes off the end first**, so what is left is the run and its warm-up and the
        // two spellings need not know about each other.
        if (const std::size_t at = text.find('@'); at != std::string_view::npos)
        {
            const std::string_view speed = text.substr(at + 1);
            const auto* end = speed.data() + speed.size();
            if (std::from_chars(speed.data(), end, mSpeed).ec != std::errc{} || !(mSpeed > 0.0f))
            {
                Log(Debug::Error) << "OPENMW_RTX_BENCH names a speed that is not a positive number: " << speed;
                mSpeed = 0.0f;
                return;
            }

            text = text.substr(0, at);
        }

        const std::size_t split = text.find(':');
        const std::optional<Span> run = readSpan(text.substr(0, split));

        // **A run nobody can read the settings of is not a run.** A spec that will not parse is a
        // typo in a benchmark somebody is about to trust, and starting anyway would hand them a
        // number for a length they did not ask for.
        if (!run.has_value() || run->empty())
        {
            Log(Debug::Error) << "OPENMW_RTX_BENCH is neither a frame count nor a duration: " << text;
            return;
        }

        mWanted = run->mFrames;
        mWantedSeconds = run->mSeconds;

        if (split != std::string_view::npos)
        {
            const Span warm = readSpan(text.substr(split + 1)).value_or(Span{});
            mWarmup = warm.mFrames;
            mWarmupSeconds = warm.mSeconds;
        }

        mHeld = std::make_unique<Held>();

        // Reserved once at a rate no frame will beat, so the run never grows a vector — a benchmark
        // that stops to reallocate is measuring its own allocator.
        mHeld->mSamples.reserve(mWanted > 0 ? mWanted : static_cast<std::uint32_t>(mWantedSeconds * 1000.0));

        Log(Debug::Info) << "Ray tracing bench: " << describeRun() << " after " << describeWarmup() << " warming up";
    }

    Bench::~Bench() = default;

    void Bench::frame(const Rtx::FrameResult& result, double frameMs, double walkMs, double placeMs, bool rebuilt)
    {
        if (mHeld == nullptr || mDone)
            return;

        // Warming up, by whichever of the two the spec named.
        if (mSeen < mWarmup || mWarmedMs < mWarmupSeconds * 1000.0)
        {
            ++mSeen;
            mWarmedMs += frameMs;
            return;
        }

        mHeld->mSamples.add(frameMs, walkMs, placeMs);
        mHeld->mSamples.addWait(result.mWaitMs);
        mHeld->mGpu.add(result.mGpu);
        mMeasuredMs += frameMs;

        fly(frameMs, rebuilt);

        const bool enough = mWanted > 0 ? mHeld->mSamples.size() >= mWanted : mMeasuredMs >= mWantedSeconds * 1000.0;
        if (!enough)
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
        if (!(mSpeed > 0.0f))
            return;

        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world.getPlayerPtr();
        if (player.isEmpty())
            return;

        // **Counted before the move below and not after it.** The move pulls the next ring in, and
        // that read lands in the frame after this one — which is the frame that arrives here
        // standing in a cell it was not drawn in last time, and the frame that paid for it.
        const void* cell = player.getCell();
        if (mCell != nullptr && cell != mCell)
        {
            ++mCrossings;
            mRebuilds += rebuilt ? 1u : 0u;
            mCrossWorstMs = std::max(mCrossWorstMs, frameMs);
        }

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

        osg::Vec3f step = ahead * (mSpeed * static_cast<float>(frameMs) / 1000.0f);
        step.z() = *mHeight - stood.pos[2];

        // **`moveObjectBy` and not `moveObject`, because the player is an actor.** The actor's
        // position lives in the physics world as well, and a move that writes only the world's copy
        // is written back over it on the next step, so the route never leaves the cell it began in.
        world.moveObjectBy(player, step, true);
    }

    std::string Bench::describeRun() const
    {
        return mWanted > 0 ? std::format("{} frames", mWanted) : std::format("{:.0f} s", mWantedSeconds);
    }

    std::string Bench::describeWarmup() const
    {
        return mWarmup > 0 ? std::format("{} frames", mWarmup) : std::format("{:.0f} s", mWarmupSeconds);
    }

    void Bench::report()
    {
        Rtx::FrameSamples& samples = mHeld->mSamples;
        const Rtx::FrameTimes frames = Rtx::summarise(samples.mFrame);
        const Rtx::FrameTimes waits = Rtx::summarise(samples.mWait);
        const Rtx::FrameTimes walks = Rtx::summarise(samples.mWalk);
        const Rtx::FrameTimes places = Rtx::summarise(samples.mPlace);
        const std::span<const Rtx::GpuZone> zones = mHeld->mGpu.summariseZones();

        // Built whole and logged once: the report is a table, and a table split across log lines by
        // a timestamp apiece is not one.
        std::string out = "\nRay tracing bench\n";
        out += Rtx::describeHeadings();
        out += Rtx::describeTimes("frame ms", frames);
        out += Rtx::describeTimes("wait ms", waits);
        out += Rtx::describeTimes("walk ms", walks);
        out += Rtx::describeTimes("place ms", places);
        out += Rtx::describeZones(zones);

        // Only where the run went somewhere, because a bench that stands still has nothing to say
        // here — the same rule `openmw-rtxtool bench` prints its crossing line under.
        if (mCrossings > 0)
            out += std::format(
                "  {} crossings, {} of them rebuilds — {:.0f} ms worst\n", mCrossings, mRebuilds, mCrossWorstMs);

        out += std::format("  {} frames in {:.2f} s — {:.1f} fps, {:.1f} at the 1% low\n", samples.size(),
            mMeasuredMs / 1000.0, frames.getRate(), frames.getLowRate());

        Log(Debug::Info) << out;
    }
}

#endif
