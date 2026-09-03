#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace RtxTool
{
    /// How often the same frame is drawn while the driver's settling is watched for, and for how
    /// long at most.
    ///
    /// **The driver finishes an acceleration structure after the build that made it, and the
    /// picture moves when it does.** Some time after `setScene` — on a thread of the driver's
    /// own, and on no signal this side can wait for — a structure starts answering the same rays
    /// with hit distances an ulp or four from before, over whole faces of a mesh, and a path
    /// tracer turns that into a different sample on a few hundred pixels. There are two pictures
    /// and never a third: before and after, and after is for good. Nothing handed to the device
    /// differs between the two — the tables, the textures, the frame block and the serialized
    /// structures were compared byte for byte. Eighty-odd processes drew a frame straight after
    /// the build, a quarter of them already settled; the same scene drawn again every second
    /// settled after one second in some processes and after four in others, in the same minute on
    /// the same card, so no pause is a bound.
    ///
    /// So the frame is drawn again every step until its picture changes, and the picture after
    /// the change is the one a comparison can be built on. A frame that holds for the whole cap
    /// is taken as already settled, which is what a process that started settled looks like —
    /// and the one thing a settling slower than the cap can be mistaken for, so a caller says
    /// which it saw. The cap is twice the slowest settling seen.
    constexpr std::chrono::milliseconds sSettleStep{ 500 };
    constexpr std::chrono::seconds sSettleCap{ 10 };

    /// Draws the same frame again every `step` until its pixels stop matching `early` or `cap`
    /// has passed, and leaves the last drawing in `settled` either way. Returns when the change
    /// was seen, in seconds from the call, or nothing where the frame held for the whole cap.
    ///
    /// @param draw fills a vector with the frame's pixels, drawn afresh.
    template <class Draw>
    std::optional<double> watchSettling(Draw&& draw, std::span<const std::uint8_t> early,
        std::vector<std::uint8_t>& settled, const std::chrono::steady_clock::duration step = sSettleStep,
        const std::chrono::steady_clock::duration cap = sSettleCap)
    {
        const auto began = std::chrono::steady_clock::now();
        for (;;)
        {
            std::this_thread::sleep_for(step);
            draw(settled);

            const auto waited = std::chrono::steady_clock::now() - began;
            if (!std::ranges::equal(settled, early))
                return std::chrono::duration<double>(waited).count();

            if (waited >= cap)
                return std::nullopt;
        }
    }

    /// How what `watchSettling` saw reads on a report line.
    inline std::string describeSettling(const std::optional<double> settledAt)
    {
        return settledAt.has_value() ? std::format("settled at {:.1f} s", *settledAt)
                                     : std::format("held for {} s", sSettleCap.count());
    }
}
