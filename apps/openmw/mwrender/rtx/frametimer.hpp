#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

#include <components/rtxbench/frametimes.hpp>

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

        /// Adds one frame's whole time to the rate. @return the window title to set, or empty until
        /// a second has run out. Never allocates: the text is written into this object's own bytes,
        /// and ends in a nought a C string wants.
        std::string_view addFrame(double frameMs);

    private:
        std::optional<std::chrono::steady_clock::time_point> mEntered;
        std::chrono::steady_clock::time_point mLeft;
        double mPresentMs = 0.0;

        Rtx::FrameRate mRate;

        /// What the window's title is written from, once a second and never allocated.
        std::array<char, 96> mTitle{};
    };
}
