#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include <components/rtxbench/framerate.hpp>

namespace MWRender
{
    /// Where one frame began and ended inside this renderer, and what it presented.
    ///
    /// **Stamps and not a report.** `Rtx::Timing::Update` is the gap between one frame leaving this
    /// renderer and the next arriving, and only the object that stamps both ends can measure it.
    /// They were four members of the renderer written at five places, and the rule that ties them —
    /// every path out stamps the leaving, and a present belongs to the span after it — was nowhere.
    class FrameSpan
    {
    public:
        /// Opens a frame. **Nothing on the first**, which has no predecessor to be timed against.
        ///
        /// @return how long since the last frame opened, in milliseconds.
        std::optional<double> enter(std::chrono::steady_clock::time_point now);

        /// Stamps where the frame left this renderer. **Every path out calls it**, so what the next
        /// frame measures is the game's own loop and never this renderer's tail.
        void leave(std::chrono::steady_clock::time_point now) { mLeft = now; }

        /// How long the game spent between the last `leave` and `now`.
        double sinceLeft(std::chrono::steady_clock::time_point now) const;

        /// Adds what one present cost.
        ///
        /// **Summed rather than kept, because a span can hold several.** The frame is measured from
        /// one trace to the next, so a present belongs to the span after it — and a loading screen
        /// drives `renderGui` as often as it likes inside one of those.
        void addPresent(double ms) { mPresentMs += ms; }

        /// What the presents since the last frame came to, and starts the sum again.
        double takePresent();

    private:
        std::chrono::steady_clock::time_point mEntered;
        bool mEnteredOnce = false;

        std::chrono::steady_clock::time_point mLeft;
        double mPresentMs = 0.0;
    };

    /// What this renderer says about its own speed: the window's title once a second, and the wait
    /// it averaged over the last few hundred frames.
    ///
    /// **The title, because this renderer has no overlay.** The rasterizer's F3 page is
    /// `osgViewer`'s and draws with it; what a window on this path can show without a frame of its
    /// own is the one line a compositor draws for it.
    class SpeedReport
    {
    public:
        /// Adds one frame's whole time. @return the title to set, or empty until a second has run
        /// out. Never allocates: the text is written into this object's own bytes.
        std::string_view addFrame(double frameMs);

        /// Adds what the CPU stood still for the device on one frame.
        ///
        /// **Counted only where a frame answered**, because `finishFrame` says nothing until a frame
        /// it put in flight comes back. Counting every frame instead divided the total by frames
        /// that had contributed nothing, so the average read low by a factor nobody could see.
        ///
        /// @return whether a line is due, which starts the sum again.
        bool addWait(double waitMs);

        /// What the frames of the line just due came to. Read after `addWait` answers true.
        double getWaitMs() const { return mSpentMs / static_cast<double>(mReported); }
        std::uint32_t getFrames() const { return mReported; }

    private:
        /// How many answered frames one line covers.
        static constexpr std::uint32_t sReportEvery = 300;

        Rtx::FrameRate mRate;

        /// What the window's title is written from, once a second and never allocated.
        std::array<char, 96> mTitle{};

        double mSpentMs = 0.0;
        std::uint32_t mTimed = 0;

        /// What the line that has just come due covers, held so the caller may read it after the
        /// sum has started again.
        std::uint32_t mReported = 0;
    };
}
