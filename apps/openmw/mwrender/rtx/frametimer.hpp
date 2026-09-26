#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include <components/rtx/latencyreport.hpp>

namespace MWRender
{
    /// Where one frame began and ended inside the ray tracer, what it presented, and what the
    /// renderer says about its own speed. Stamps and not a report: `Rtx::Timing::Update` is the
    /// gap between one frame leaving the renderer and the next arriving, and only the object that
    /// stamps both ends can measure it.
    class FrameTimer
    {
    public:
        /// Opens a frame. @return how long since the last frame opened, in milliseconds, and nothing
        /// on the first. Every frame the renderer is handed opens one, traced or not.
        std::optional<double> enter(std::chrono::steady_clock::time_point now);

        /// Stamps where the frame left the renderer. Every path out calls it, so what the next frame
        /// measures is the game's own loop and never the renderer's tail.
        void leave(std::chrono::steady_clock::time_point now) { mLeft = now; }

        /// How long the game spent between the last `leave` and `now`.
        double sinceLeft(std::chrono::steady_clock::time_point now) const;

        /// Adds what one present cost. Summed, because a loading screen presents as often as it
        /// likes inside one span.
        void addPresent(double ms) { mPresentMs += ms; }

        /// What the presents since the last frame came to, and starts the sum again.
        double takePresent();

        /// Adds one frame's whole time to the second being counted. @return whether that second has
        /// run out, which is when the title is written again.
        ///
        /// **A second and not a frame**, because a figure that changes sixty times a second cannot be
        /// read. **The mean beside the worst**, because the mean alone is the figure that hides a
        /// stutter. A median would need the second's frames kept and sorted, and a sort landing on
        /// one frame in sixty is the spike a frame path is not allowed to carry.
        bool addFrame(double frameMs);

        /// The window title for the second that ran out. Never allocates: the text is written into
        /// this object's own bytes, and ends in a nought a C string wants.
        ///
        /// @param latency the driver's newest timings, where the driver paces: the title carries
        ///        the input-to-present figure beside the rate, and nothing where there is none.
        /// @param note what the run says of where it stands, after the rest, or empty for nothing.
        std::string_view writeTitle(const std::optional<Rtx::LatencyReport>& latency, std::string_view note);

    private:
        std::optional<std::chrono::steady_clock::time_point> mEntered;
        std::chrono::steady_clock::time_point mLeft;
        double mPresentMs = 0.0;

        /// The second being counted.
        double mSummedMs = 0.0;
        double mWorstMs = 0.0;
        std::uint32_t mFrames = 0;

        /// The last second that ran out, which the title describes: no frames until one has.
        double mSecondMeanMs = 0.0;
        double mSecondWorstMs = 0.0;

        /// What the window's title is written from, once a second and never allocated.
        std::array<char, 128> mTitle{};
    };
}
