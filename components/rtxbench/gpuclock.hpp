#pragma once

#include <cstdint>
#include <string>

#include <components/rtx/monitor.hpp>
#include <components/rtx/worker.hpp>

namespace Rtx
{
    /// What the device's clock and power state were, as `nvidia-smi` reports them.
    ///
    /// **A frame time without its clock is not a number to compare.** A card under load is held at
    /// whatever its power budget allows: this one runs about 1.8 GHz there against 2.3 GHz cool, so
    /// the same build measures several per cent apart from one run to the next. A run that says
    /// which clock it was taken at can be held against a run taken on another day.
    ///
    /// **A range and not a reading, because a card moves while a place is measured.** One sample is
    /// one moment, and a moment sampled after the frames stopped is a card already climbing back —
    /// which reads as a fast clock over frames drawn at a slower one.
    ///
    /// **And a range of two is not a range.** The ends of a place were once the whole of this, and
    /// two samples cannot say what the clock did between them: measured at 10 Hz through a
    /// `seyda-neen-ship` bench the core moves 9 to 22% across the settled part of a run, while the
    /// two ends agreed to within 2% and the row read `1890–1920 MHz`. `ClockWatch` is what samples
    /// through the frames instead, and `mReadings` is what says whether a range was worth printing.
    struct GpuClock
    {
        /// The graphics clock over every reading taken. Equal where only one was.
        std::uint32_t mLowestMhz = 0;
        std::uint32_t mHighestMhz = 0;

        /// Every reading's core clock summed, and how many there were.
        ///
        /// **The mean is what a frame time is read against**, where the two ends are what says
        /// whether the card moved at all. A leg whose mean is below its neighbours' is the leg to
        /// repeat, and a low that only one reading of a hundred saw is noise rather than a run.
        std::uint64_t mSumMhz = 0;
        std::uint32_t mReadings = 0;

        std::uint32_t mMemoryMhz = 0;

        /// The highest any reading saw, which is the one that explains a throttle.
        std::uint32_t mTemperatureC = 0;

        /// Why the card was not running faster, as NVML's own bits, or-ed over every reading.
        /// `describeThrottle` turns it into words.
        std::uint64_t mThrottleMask = 0;

        /// False where nothing answered — no `nvidia-smi`, another vendor's device, or an answer
        /// this cannot read. Such a run reports no clock rather than a made-up one.
        bool mRead = false;

        /// Takes `other` in: the clock spans both, and the reasons are what either saw. A reading
        /// that answered nothing adds nothing, so a machine that answers once and then not again
        /// still reports the once.
        void add(const GpuClock& other);

        /// One reading, with the range, the sum and the count that one reading implies.
        ///
        /// **Named, because five fields say "one reading" together.** The ends are that reading, the
        /// sum is it, and the count is one — and a caller that set four of the five would report a
        /// mean of nought over a clock it had just read.
        static GpuClock reading(
            std::uint32_t coreMhz, std::uint32_t memoryMhz, std::uint32_t temperatureC, std::uint64_t throttle);

        /// The mean core clock, or nought where nothing answered.
        std::uint32_t getMeanMhz() const { return mReadings > 0 ? static_cast<std::uint32_t>(mSumMhz / mReadings) : 0; }
    };

    /// Asks the device what it is doing now, as one reading.
    ///
    /// **A process spawn, so it is never asked on a frame path.** A frame that waited for
    /// `nvidia-smi` would be the worst frame of the run and would say so in the p99. `ClockWatch` is
    /// what asks it repeatedly, on a thread of its own.
    GpuClock readGpuClock();

    /// The clock through a place's frames rather than at their ends.
    ///
    /// **A thread of its own, because the reading is a process spawn.** The frame path never waits
    /// for one, which is the rule `readGpuClock` states — and what this adds is that nobody has to
    /// choose between waiting and not asking.
    ///
    /// **Every reading forks this process**, and a harness with a world loaded is a large one to
    /// fork, so the rate is what the spawn cost was measured under rather than what the card can be
    /// asked for. At four a second the trace zone at `seyda-neen-ship` reads 1.66 to 1.68 ms over
    /// five legs, against 1.62 to 1.67 over the sixteen taken before this existed.
    ///
    /// **One of these outlives a place**, so `start` is what forgets the last one's readings.
    class ClockWatch
    {
    public:
        ClockWatch() = default;
        ~ClockWatch();

        ClockWatch(const ClockWatch&) = delete;
        ClockWatch& operator=(const ClockWatch&) = delete;

        /// Forgets what the last place saw and starts sampling, taking one reading straight away so
        /// a place that ends at once still answers. Nothing at all where one is already running,
        /// which includes forgetting: that run's readings are its own.
        void start();

        /// Stops sampling and answers everything it saw, this call's own last reading included.
        GpuClock stop();

        /// How many readings the run now open has taken, which `stop` then adds its own to.
        ///
        /// **The number a caller waits on rather than a clock it guesses at.** A reading forks a
        /// process, so how long a loop turn takes is the machine's to say — and a sleep chosen for
        /// the slowest box this might run on is a wait every other box pays.
        std::uint32_t getReadings();

    private:
        /// The lock over `mSeen`, and nothing else: there is no channel here, because a sampler
        /// hands nothing over until it is stopped.
        Monitor mMonitor;

        /// What every reading so far came to, under the lock.
        GpuClock mSeen;

        /// **Last, for the reason `Worker` gives.**
        Worker mWorker;
    };

    /// The clock as one line of the report, or empty where nothing answered.
    std::string describeClock(const GpuClock& clock);

    /// What a throttle mask names, in the order NVML's bits are numbered, or empty for a card that
    /// nothing is holding back.
    std::string describeThrottle(std::uint64_t mask);
}
