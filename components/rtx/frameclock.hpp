#pragma once

#include <chrono>
#include <optional>

namespace Rtx
{
    /// Milliseconds between two readings of the steady clock, which is what every timed figure in
    /// this fork is. Beside the clock and not beside the report, because a backend timing a wait
    /// should not reach the report to subtract two time points.
    inline double since(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
    {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    /// How long a frame stands for, and what time it is once it has. One clock for a run, because
    /// a run that reads two cannot repeat itself: `SceneManager::checkLoaded` and
    /// `CellPreloader::isTerrainLoaded` decide by the reference time, so two runs on two clocks
    /// are handed different worlds. A measured run states a step and what ages is the frame
    /// index; a played session is handed the wall, so a stall ages a player's caches and not a
    /// benchmark's. How long since the last *traced* frame is `Rtx::FrameOptions::mSinceLast`,
    /// because a loading screen drives frames that draw no world.
    class FrameClock
    {
    public:
        /// @param step how long every frame stands for, or nothing to follow the wall.
        explicit FrameClock(std::optional<float> step = std::nullopt)
            : mFixed(step)
        {
        }

        /// Opens the next frame.
        ///
        /// @param measured what the wall says the last frame took, in seconds, which a clock with a
        ///        step of its own ignores.
        void advance(const double measured)
        {
            mStep = mFixed.has_value() ? static_cast<double>(*mFixed) : measured;

            // **Sampled once a frame rather than read per question**, so two callers asking what
            // time it is inside one frame cannot be told two things.
            mNow = mFixed.has_value() ? mNow + mStep
                                      : std::chrono::duration<double>(std::chrono::steady_clock::now() - mMade).count();
        }

        /// How long the frame now open stands for, in seconds. Nought before the first `advance`.
        double getStep() const { return mStep; }

        /// What time it is, in seconds. The frames counted where a step is stated, and what the
        /// wall says since this clock was made where none is.
        double getNow() const { return mNow; }

        /// The step a run stated, or nothing where the wall decides.
        ///
        /// **What says the run repeats itself**, and what `Rtx::FrameOptions::mSinceLast` takes.
        /// The absence is the information: `getStep` answers either way, and only this says which.
        std::optional<float> getStatedStep() const { return mFixed; }

    private:
        std::optional<float> mFixed;

        /// What `getNow` counts from, where the wall decides.
        std::chrono::steady_clock::time_point mMade = std::chrono::steady_clock::now();

        double mStep = 0.0;
        double mNow = 0.0;
    };
}
