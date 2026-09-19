#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <components/rtx/framedigest.hpp>
#include <components/rtx/shaders/digest.h>

namespace Rtx
{
    /// When a measured run may begin: once the driver's second compile of the launches is over,
    /// which the process's own clocks say.
    ///
    /// **The driver compiles the launches twice, and the second code is not the first.** Once when
    /// a pipeline is made, and again on a thread of its own — one it makes with `clone` and not
    /// `pthread_create`, unnamed, at a core flat out for twenty seconds from two seconds after the
    /// launches were made — swapping each launch's code in as it is done. `camera.h` holds the
    /// ray's direction steady across the two with `precise`, and the direction is the same to the
    /// bit; what the second code contracts differently is the arithmetic past it — the clip depth
    /// by an ulp on a few texels, the motion vectors through the cancellation in `reprojected`,
    /// the shading on a few — and every column the trace writes moves with it. Two runs measured
    /// across the swap differ from the frame one swapped to the frame the other did, and agree
    /// again after, and a frame timed across it is timed on two codes. Nothing turns it off: not
    /// `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`, not
    /// `__GL_THREADED_OPTIMIZATIONS=0`, not launches created from `VK_KHR_pipeline_binary`'s
    /// binaries, which hash the same before and after the swap and swap just the same; not a
    /// `VkDeferredOperationKHR` joined by one thread or four, which the driver defers and finishes
    /// in the time a plain creation takes, the thread running after it as before; and
    /// `__GL_SINGLE_THREADED=1` faults the process at the instance. What the driver's own disk
    /// cache holds is the second code: a process whose launches come out of it starts on it, with
    /// no thread to wait for, and drew every frame the same as one that compiled and settled. So
    /// the first process after a shader changes settles, and the ones after it find nothing to.
    ///
    /// **So a run traces one frame over and over until the thread is done.** The renderer traces
    /// the frame it was handed again, inside the same game frame, with every history reset before
    /// each trace — so every trace is the same trace and its digest is the same digest until the
    /// code changes, and the world moves by nothing while it waits. The thread is not the
    /// process's to see by name, so its work is read off the process's CPU clock less the tracing
    /// thread's: a window of `sWindowSeconds` settles the run when the process's other threads
    /// took under `sBusyShare` of it and the digest did not move inside it. A run that has no such
    /// window by `capSeconds` measures anyway, and the description says so. What the frame showed
    /// of the swap stands beside it, if it showed anything: an interior's frame can show nothing.
    class CodeSettle
    {
    public:
        using Traced = std::array<DigestWords, Shaders::DIGEST_IMAGES>;

        /// Two seconds, so that a second at least stands between the thread's last work and the
        /// first measured frame — the window that holds its finish is judged on the whole of it —
        /// and the analysis thread the driver answers every submission on averages out.
        static constexpr double sWindowSeconds = 2.0;

        /// Half a core. The compile holds a core flat out, a hundred jiffies a second measured,
        /// and the analysis thread takes a tenth of one at most, nine measured; half sits between
        /// with room both ways.
        static constexpr double sBusyShare = 0.5;

        explicit CodeSettle(double capSeconds);

        /// One trace's digest, the wall clock since the settle began, and the CPU time of every
        /// thread of the process but the tracing one — `otherThreadsCpuSeconds`, or nothing where
        /// the platform keeps no such clock, which leaves the cap to settle the run.
        void take(const Traced& traced, double seconds, std::optional<double> othersCpuSeconds);

        bool isSettled() const;

        /// What the settle came to, for the run's report.
        std::string describe() const;

        /// The process's CPU time less the calling thread's, or nothing where the platform keeps
        /// no process clock.
        static std::optional<double> otherThreadsCpuSeconds();

    private:
        /// Where the window being judged began.
        struct Anchor
        {
            double mSeconds;
            double mCpu;
        };

        double mCapSeconds;

        std::optional<Anchor> mWindow;
        bool mMovedInWindow = false;
        std::optional<double> mSettledAt;
        double mFirstCpu = 0.0;
        double mCpu = 0.0;

        std::optional<Traced> mLast;
        std::uint32_t mTraces = 0;
        std::uint32_t mChanges = 0;
        std::uint32_t mChangedAt = 0;
        std::array<bool, Shaders::DIGEST_IMAGES> mMoved{};
        double mSeconds = 0.0;
    };
}
