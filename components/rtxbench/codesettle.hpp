#pragma once

#include <optional>
#include <string>
#include <vector>

#include "threadwatch.hpp"

namespace Rtx
{
    /// When a process that compiled the launches may be trusted to have a warm driver cache:
    /// once the driver's second compile of them is over, which the process's own threads say.
    ///
    /// **The driver compiles the launches twice, and the second code is not the first.** Once
    /// when a pipeline is made, and again on an unnamed thread of its own, eighteen to twenty-two
    /// seconds of a core from the first pipelines' creation, swapping each launch's code in
    /// as it is done and writing its disk cache as it goes. Nothing turns it off. What the driver's
    /// disk cache holds is the second code: a process whose launches come out of it starts on it,
    /// with no thread to wait for, and draws every frame the same as one that compiled and waited.
    /// So the process that compiled waits this out drawing frames, then starts again, and the one
    /// that starts again measures — `RtxTool::Session` runs that.
    ///
    /// **A verdict over `ThreadWindows`.** The thread is not the process's to see by name, so it
    /// is read as the busiest of the process's other threads, window by window. `sQuietWindows`
    /// windows in a row under `sBusyShare` settle the run — after a window over it, because the
    /// thread pauses for a second or more while the frames upload the world, and a window that
    /// saw nothing has not seen it finish; two, because one quiet window was a pause: a prime that
    /// left on it handed the next process a cache short of the last second of the compile, and
    /// that process ran its whole measured stretch on the first code of the launches the second
    /// never reached. A run with no such windows by `capSeconds` settles anyway, and `wentQuiet`
    /// and the description say so.
    class CodeSettle
    {
    public:
        /// A quarter of a core. The compile holds a core flat out, and half of one across a
        /// window it paused inside; a worker of the game's takes a tenth at most.
        static constexpr double sBusyShare = 0.25;

        static constexpr int sQuietWindows = 2;

        explicit CodeSettle(double capSeconds);

        /// Reads the verdict off `windows` as they stand. Nothing where a platform keeps no view
        /// of the threads, which leaves the cap to settle the run.
        void judge(const ThreadWindows& windows);

        bool isSettled() const;

        /// Whether the settle saw the thread finish, which is the one settle a cache can be
        /// trusted after: a cap that stood is a compile that did not end, and a process that
        /// starts again on it compiles again.
        bool wentQuiet() const { return mSettledAt.has_value(); }

        /// What the settle came to, for the run's report.
        std::string describe() const;

    private:
        double mCapSeconds;

        /// Every window's share as last judged, for the report: what a cap that stood was
        /// looking at.
        std::vector<double> mShares;

        bool mView = false;
        bool mSeenBusy = false;
        std::optional<double> mSettledAt;
        double mBusiestTotal = 0.0;
        double mSeconds = 0.0;
    };
}
