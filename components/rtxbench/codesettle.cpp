#include "codesettle.hpp"

#include <cstddef>
#include <format>

#include "framehashes.hpp"

#ifdef __linux__
#include <time.h>
#endif

namespace Rtx
{
    CodeSettle::CodeSettle(const double capSeconds)
        : mCapSeconds(capSeconds)
    {
    }

    void CodeSettle::take(const Traced& traced, const double seconds, const std::optional<double> othersCpuSeconds)
    {
        if (mSettledAt.has_value())
            return;

        ++mTraces;
        mSeconds = seconds;

        if (mLast.has_value() && *mLast != traced)
        {
            ++mChanges;
            mChangedAt = mTraces;
            mMovedInWindow = true;
            for (std::size_t image = 0; image < traced.size(); ++image)
                mMoved[image] = (*mLast)[image] != traced[image];
        }
        mLast = traced;

        if (!othersCpuSeconds.has_value())
            return;

        mCpu = *othersCpuSeconds;
        if (!mWindow.has_value())
        {
            mFirstCpu = mCpu;
            mWindow = Anchor{ .mSeconds = seconds, .mCpu = mCpu };
            return;
        }

        const double length = seconds - mWindow->mSeconds;
        if (length < sWindowSeconds)
            return;

        const bool quiet = (mCpu - mWindow->mCpu) / length < sBusyShare && !mMovedInWindow;
        if (quiet)
            mSettledAt = seconds;

        mWindow = Anchor{ .mSeconds = seconds, .mCpu = mCpu };
        mMovedInWindow = false;
    }

    bool CodeSettle::isSettled() const
    {
        return mSettledAt.has_value() || mSeconds >= mCapSeconds;
    }

    std::string CodeSettle::describe() const
    {
        std::string threads;
        if (!mWindow.has_value())
            threads = std::format(
                "no clock of the process's other threads on this platform, so the cap of {:.0f} s stood", mCapSeconds);
        else if (mSettledAt.has_value())
            threads = std::format("the process's other threads went quiet at {:.1f} s after {:.1f} s of CPU",
                *mSettledAt, mCpu - mFirstCpu);
        else
            threads = std::format(
                "the process's other threads had no quiet window in {:.1f} s ({:.1f} s of CPU), so the cap stood",
                mSeconds, mCpu - mFirstCpu);

        if (mChanges == 0)
            return std::format("{}; the launches' code did not change in {} traces", threads, mTraces);

        std::string moved;
        for (std::size_t image = 0; image < mMoved.size(); ++image)
            if (mMoved[image])
                moved += std::format("{}{}", moved.empty() ? "" : ", ", tracedName(image));

        const std::string times = mChanges == 1 ? "" : std::format("{} times, last ", mChanges);
        return std::format(
            "{}; the launches' code changed {}at trace {} of {} — {}", threads, times, mChangedAt, mTraces, moved);
    }

    std::optional<double> CodeSettle::otherThreadsCpuSeconds()
    {
#ifdef __linux__
        timespec process{};
        timespec thread{};
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &process) != 0
            || clock_gettime(CLOCK_THREAD_CPUTIME_ID, &thread) != 0)
            return std::nullopt;

        return static_cast<double>(process.tv_sec - thread.tv_sec)
            + static_cast<double>(process.tv_nsec - thread.tv_nsec) * 1e-9;
#else
        return std::nullopt;
#endif
    }
}
