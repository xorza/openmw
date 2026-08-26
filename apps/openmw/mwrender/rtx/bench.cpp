#include "bench.hpp"

#ifdef OPENMW_RTX_BENCH

#include <charconv>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/rtx/frametimes.hpp>
#include <components/rtx/renderer.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/statemanager.hpp"

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
        std::vector<double> mFrames;
        std::vector<double> mTraces;
        Rtx::GpuBreakdown mGpu;
    };

    Bench::Bench()
    {
        const char* const spec = std::getenv("OPENMW_RTX_BENCH");
        if (spec == nullptr || *spec == '\0')
            return;

        const std::string_view text(spec);
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
        const std::size_t room = mWanted > 0 ? mWanted : static_cast<std::size_t>(mWantedSeconds * 1000.0);
        mHeld->mFrames.reserve(room);
        mHeld->mTraces.reserve(room);

        Log(Debug::Info) << "Ray tracing bench: " << describeRun() << " after " << describeWarmup() << " warming up";
    }

    Bench::~Bench() = default;

    void Bench::frame(const Rtx::FrameResult& result, double frameMs)
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

        mHeld->mFrames.push_back(frameMs);
        mHeld->mTraces.push_back(result.mTraceMs);
        mHeld->mGpu.add(result.mGpu);
        mMeasuredMs += frameMs;

        const bool enough = mWanted > 0 ? mHeld->mFrames.size() >= mWanted : mMeasuredMs >= mWantedSeconds * 1000.0;
        if (!enough)
            return;

        report();
        mDone = true;

        // **The way the quit key ends a session, and not `exit`.** A run that tore the process down
        // where it stood would leave the save, the log and the device wherever they happened to be,
        // and the next thing anyone would debug is the benchmark.
        MWBase::Environment::get().getStateManager()->requestQuit();
    }

    std::string Bench::describeRun() const
    {
        return mWanted > 0 ? std::format("{} frames", mWanted) : std::format("{:.0f} s", mWantedSeconds);
    }

    std::string Bench::describeWarmup() const
    {
        return mWarmup > 0 ? std::format("{} frames", mWarmup) : std::format("{:.0f} s", mWarmupSeconds);
    }

    void Bench::report() const
    {
        const Rtx::FrameTimes frames = Rtx::summarise(mHeld->mFrames);
        const Rtx::FrameTimes traces = Rtx::summarise(mHeld->mTraces);
        const std::span<const Rtx::GpuZone> zones = mHeld->mGpu.summariseZones();

        // Built whole and logged once: the report is a table, and a table split across log lines by
        // a timestamp apiece is not one.
        std::string out = "\nRay tracing bench\n";
        out += Rtx::describeHeadings();
        out += Rtx::describeTimes("frame ms", frames);
        out += Rtx::describeTimes("trace ms", traces);
        out += Rtx::describeZones(zones);
        out += std::format("  {} frames in {:.2f} s — {:.1f} fps, {:.1f} at the 1% low\n", mHeld->mFrames.size(),
            mMeasuredMs / 1000.0, frames.getRate(), frames.getLowRate());

        Log(Debug::Info) << out;
    }
}

#endif
