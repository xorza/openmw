#pragma once

#include <chrono>
#include <optional>

namespace Rtx
{
    /// Milliseconds between two readings of the steady clock, which is what every timed figure in
    /// this fork is.
    ///
    /// **One spelling, because it was written seven times.** `std::chrono::duration<double,
    /// std::milli>(to - from).count()` says nothing a reader needs and hides which way round the
    /// subtraction goes; every timed stretch in this fork now reads the same way.
    ///
    /// **Beside the clock rather than beside the report it feeds.** A backend times a wait and the
    /// game times a walk; neither of them summarises a run, and neither should reach the report to
    /// subtract two time points.
    inline double since(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
    {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    /// How long a frame stands for, and what time it is once it has.
    ///
    /// **One clock for a run, because a run that reads two cannot repeat itself.** How far the
    /// simulation steps, how long the renderer is told the frame took, and what OpenMW ages its
    /// caches and its preloads by are three questions with one answer. They used to be three reads
    /// of `[RTX] fixed step`, each falling back to a clock of its own: `Engine::go` took the
    /// frame-rate limiter, `RtxRenderer::advance` took `osg::Timer` and `VulkanRenderer::renderFrame`
    /// took `std::chrono`.
    ///
    /// **What that cost, measured.** `MWWorld::Scene` reads the reference time eight times, and two
    /// of those decide rather than expire — `SceneManager::checkLoaded` says whether an object's
    /// mesh is already in hand, and `CellPreloader::isTerrainLoaded` says whether a grid change may
    /// go ahead. Two runs of one build over `island-crossing` were handed different worlds: 5,776
    /// placed instances against 5,772, and 467 textures against 466.
    ///
    /// **A step, or the wall.** A measured run states how long every frame stands for and this
    /// counts them, so what ages is the frame index. A played session states nothing, so it is
    /// handed what the wall said and the time it reports is the wall's — a stall should age a
    /// player's caches, and it should not age a benchmark's.
    ///
    /// **What this does not answer is how long since the last *traced* frame.** A loading screen
    /// drives frames that draw no world, so the interval a motion vector belongs to is not this
    /// step. `Rtx::FrameOptions::mSinceLast` says who measures that where no run states one.
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
