#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <components/rtx/monitor.hpp>
#include <components/rtx/worker.hpp>

#include "gpuclock.hpp"
#include "nvml.hpp"

namespace RtxTool
{
    /// One process other than this one that held the card, and in how many of the driver's
    /// samples it did.
    struct CardHolder
    {
        std::uint32_t mPid = 0;
        std::string mName;
        std::uint32_t mSamples = 0;
    };

    /// Who held the card across a window of a run, by the driver's own samples.
    ///
    /// **What says whether a place's figures are the renderer's.** A compositor's frame, an
    /// editor's redraw, a browser's tab: each preempts the card for milliseconds, lands on
    /// whichever pass is running, and the frame that carries it reads as the renderer's. Every
    /// figure moves with it, the median of a host row as readily as the tail of the frame. The
    /// driver samples five times a second which process has the card, and this counts the
    /// samples that were somebody else's — one in twenty at a four per cent share, so a short
    /// place under a light load may see none, and a run that saw any was under a load.
    struct CardShare
    {
        /// How long the window was.
        double mSeconds = 0.0;

        /// The driver's samples in which some process had the card busy, and those in which a
        /// process other than this one did.
        std::uint32_t mSamples = 0;
        std::uint32_t mOthers = 0;

        /// The others, most samples first.
        std::vector<CardHolder> mHolders;

        /// False where the driver keeps no such samples, with why.
        bool mViewed = false;
        std::string_view mWhyNot;
    };

    /// The tally the samples go into. Apart from the watch that feeds it, so what it counts can
    /// be proved without a driver.
    class CardTally
    {
    public:
        /// `self` is this process as the driver numbers it: its own samples are counted and are
        /// nobody else's.
        explicit CardTally(std::uint32_t self)
            : mSelf(self)
        {
        }

        /// One sample in which `pid`, called `name`, had the card busy. The name is kept from the
        /// first sample of a process, so one that is gone by the report is still named.
        void take(std::uint32_t pid, std::string_view name);

        /// Forgets every sample; the process stays.
        void clear();

        /// What the samples since `clear` came to, over a window of `seconds`.
        CardShare summarise(double seconds) const;

    private:
        std::uint32_t mSelf;
        std::uint32_t mSamples = 0;
        std::uint32_t mOthers = 0;
        std::vector<CardHolder> mHolders;
    };

    /// What a place's window came to: the clock through it and who held the card.
    struct CardReading
    {
        GpuClock mClock;
        CardShare mShare;
    };

    /// The card through a run, on a thread of its own: its clock across a place's frames, and
    /// who held it through every window of the run.
    ///
    /// **The clock sampled through the frames rather than at their ends**, because two ends
    /// agree to within a couple of per cent while the card moves a fifth of its clock between
    /// them, and a leg that lost its clock then reads like a leg that lost its speed. **The
    /// holders from the process's start and not from the frames**, because the window before
    /// the first place — the load, at the card's idle clock, where a redraw lasts long enough to
    /// be caught in nearly every sample — is where an animating desktop shows plainest.
    class CardWatch
    {
    public:
        CardWatch();
        ~CardWatch();

        /// Starts sampling. Nothing where it already is.
        void watch();

        /// Begins a place's window, forgetting the clock's readings and the holders, and answers
        /// the holders seen since `watch` or the last `stop`: the window between places.
        CardShare start();

        /// Ends the place's window and answers what it came to, this call's own last reading
        /// included; sampling carries on into the window after.
        CardReading stop();

        /// How many clock readings the window now open has taken: the number a caller waits on
        /// rather than a sleep chosen for the slowest box this might run on.
        std::uint32_t getReadings();

    private:
        /// One reading of both, under the lock: the reading is a few calls into the driver,
        /// microseconds, so the lock is cheaper than a copy.
        void read();

        /// Takes one last reading, answers the window and begins the next. Under the lock.
        CardReading close();

        Nvml mNvml;

        /// The lock over everything below but the worker.
        Rtx::Monitor mMonitor;

        GpuClock mClock;
        CardTally mTally;
        std::chrono::steady_clock::time_point mBegan;

        /// The sampler's scratch, refilled a reading.
        std::vector<CardSample> mSamples;
        std::string mName;

        /// Last, for the reason `Worker` gives.
        Rtx::Worker mWorker;
    };

    /// The share as one line of the report, without the indent and the line break, so a caller
    /// can say which window it was.
    std::string describeCard(const CardShare& share);
}
