#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <components/rtx/monitor.hpp>
#include <components/rtx/worker.hpp>

namespace Rtx
{
    /// One thread of the process and how much CPU it has had so far.
    struct ThreadCpu
    {
        std::uint64_t mThread = 0;
        double mSeconds = 0.0;
    };

    /// What a place's `ThreadWindows` came to, for its record: the window whose busiest thread
    /// took the most, how many there were, and the longest stretch of them a thread held a core
    /// through. A place with none closed says so by the count.
    ///
    /// **The driver's compile is told from the game's own threads by how long it holds a core,
    /// not by how much of one.** The game's cell loader is an unnamed thread like the driver's,
    /// and on the fastest route it took 0.81 of a core across two windows; the guild's load took
    /// 0.51 across one. The compile holds a core flat out, 1.0 a window, for ten seconds and more,
    /// and about half of one across a window it paused inside. So a window at `sFlatOutShare` is
    /// one the loader has not reached, and `sCompileWindows` of them in a row is a stretch no load
    /// has held: the signature `Check::DriverQuiet` asserts against.
    struct ThreadShare
    {
        static constexpr double sFlatOutShare = 0.9;
        static constexpr std::uint32_t sCompileWindows = 3;

        /// The largest share of a core one thread had across any window, and the clock that
        /// window closed at.
        double mLargestShare = 0.0;
        double mLargestAt = 0.0;

        std::uint32_t mWindows = 0;

        /// The most windows in a row whose busiest thread was at `sFlatOutShare` or over.
        std::uint32_t mLongestFlatOut = 0;

        /// False where the platform keeps no view of the process's threads.
        bool mViewed = false;
    };

    /// The thread share as one line of the report.
    std::string describeThreads(const ThreadShare& share);

    /// The busiest of a process's other threads, window by window: how much of a core the one
    /// thread that took the most had across each `sWindowSeconds` of a run.
    ///
    /// **One thread and never their sum.** The thread this exists to see is the driver's second
    /// compile of the launches (`CodeSettle`), a core flat out for ten seconds and more; the
    /// game's own workers take a quarter of a core between them while a world is drawn, and a
    /// thread that lives for a frame is in the process's clock and in no thread's. So a thread is
    /// matched across a window by its id, and a window's figure is the largest one thread's.
    ///
    /// Fed readings in order; a caller with a thread of its own to take them is `ThreadWatch`.
    class ThreadWindows
    {
    public:
        /// One window closed: the share of a core its busiest thread had, and the clock it closed
        /// at.
        struct Window
        {
            double mShare = 0.0;
            double mSeconds = 0.0;
        };

        static constexpr double sWindowSeconds = 2.0;

        /// The least a window closed by `finish` may span: a run shorter than a window still
        /// answers, and a sliver of a tenth of a second answers nothing.
        static constexpr double sShortestWindowSeconds = 0.5;

        /// One reading: the wall clock since the windows began, and every other thread of the
        /// process with its CPU so far, sorted by thread. Closes a window where the last one
        /// closed `sWindowSeconds` ago. Empty `threads` is a platform with no such view, which
        /// closes nothing.
        void take(double seconds, std::span<const ThreadCpu> threads);

        /// The last reading, closing the window still open if it spans `sShortestWindowSeconds`.
        void finish(double seconds, std::span<const ThreadCpu> threads);

        /// Every window closed, in order.
        std::span<const Window> getWindows() const { return mClosed; }

        /// The last reading's clock.
        double getSeconds() const { return mSeconds; }

        /// Whether any reading had threads in it: false on a platform with no view of them.
        bool hasView() const { return mWindow.has_value(); }

        /// The busiest thread's seconds summed over every window.
        double getBusiestTotal() const { return mBusiestTotal; }

        /// What a place records of this.
        ThreadShare summarise() const;

    private:
        /// Where the window being judged began: when, and what every thread had had by then.
        struct Anchor
        {
            double mSeconds = 0.0;
            std::vector<ThreadCpu> mThreads;
        };

        void close(double seconds, std::span<const ThreadCpu> threads);

        /// The most any one thread took from the anchor to `threads`.
        double busiestSeconds(std::span<const ThreadCpu> threads) const;

        std::optional<Anchor> mWindow;
        std::vector<Window> mClosed;
        double mSeconds = 0.0;
        double mBusiestTotal = 0.0;
    };

    /// `ThreadWindows` fed from a thread of its own, four times a second, so no frame of the run
    /// reads thirty files under `/proc` — the reading is tens of microseconds a thread and the
    /// frame path is measured. One of these outlives a place, so `start` is what forgets the last
    /// place's windows.
    class ThreadWatch
    {
    public:
        ThreadWatch() = default;
        ~ThreadWatch();

        /// Forgets what the last place saw and starts sampling. `except` is the thread whose work
        /// is the run's own, left out beside the sampler's: `currentThread` from the frame's
        /// thread. Nothing where one is already running.
        void start(std::uint64_t except);

        /// Runs `read` under the lock with the windows as they stand, for a caller that judges
        /// them while the run goes on. Nothing is copied.
        template <class Read>
        auto under(Read read)
        {
            return mMonitor.under([&] { return read(static_cast<const ThreadWindows&>(mWindows)); });
        }

        /// Stops sampling and answers everything it saw, this call's own last reading included.
        ThreadWindows stop();

        /// The calling thread's id as `readOtherThreads` names one, or nought on a platform
        /// without the view.
        static std::uint64_t currentThread();

        /// Every thread of the process but the calling one and `except`, sorted by thread, into
        /// `into` — cleared and refilled, so a caller keeps one. Empty where the platform keeps no
        /// such view.
        static void readOtherThreads(std::vector<ThreadCpu>& into, std::uint64_t except);

    private:
        void read();

        Monitor mMonitor;
        ThreadWindows mWindows;

        /// The sampler's scratch, refilled a reading, and whose thread it leaves out.
        std::vector<ThreadCpu> mThreads;
        std::uint64_t mExcept = 0;
        std::chrono::steady_clock::time_point mBegan;

        /// Last, for the reason `Worker` gives.
        Worker mWorker;
    };
}
