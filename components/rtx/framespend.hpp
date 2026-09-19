#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ratio>
#include <string_view>
#include <utility>

#include "namedenum.hpp"

namespace Rtx
{
    /// Milliseconds between two readings of the steady clock, which is what every timed figure in
    /// this fork is. Beside the figures and not beside the report, because a backend timing a wait
    /// should not reach the report to subtract two time points.
    inline double since(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
    {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    /// Which of a measured frame's figures a row holds. `Wait` is the CPU standing still for the
    /// device — a wait near the frame is a device that cannot keep up, near nought a CPU that
    /// cannot — and `Finish` is the whole of collecting the frame behind, of which `Wait` is the
    /// largest share. `Walk` is the world being mirrored, with `Fold` the share of it spent
    /// folding geometry that arrived on it. `Place` is the renderer being told what moved, split
    /// into `Bake`, `Textures` and `Upload` because its worst frame is hundreds of times its
    /// median and a profile cannot say which half. `Trace` and `Present` are the other two calls
    /// into the backend, and `Update` is the rest of the loop, which is the game's. Timed rather
    /// than profiled, because most of what a call into the driver costs is inside the driver with
    /// no frame pointer to walk, and a thread asleep is nothing to a sampling profiler. Together
    /// they close the frame: `Frame` less the rest is under 0.05 ms at every place that stands
    /// still, and a row that does not close is a stretch nobody has named.
    enum class Timing : std::uint32_t
    {
        Frame,
        Finish,
        Wait,
        Walk,
        Fold,
        Place,
        Bake,
        Textures,
        Upload,
        Trace,

        /// The pictures inside the interface drawn this frame — a doll's walk and placement, a map
        /// tile's recording — which stand between the placement and the trace.
        Views,
        Present,
        Update,
    };

    /// What a report heads each row with, and — with `Ms` after it — what the JSON names it. One
    /// table, from which the count and the walk are derived (`Rtx::NamedEnum`).
    inline constexpr NamedEnum sTimings{ std::array{
        std::pair{ Timing::Frame, std::string_view("frame") },
        std::pair{ Timing::Finish, std::string_view("finish") },
        std::pair{ Timing::Wait, std::string_view("wait") },
        std::pair{ Timing::Walk, std::string_view("walk") },
        std::pair{ Timing::Fold, std::string_view("fold") },
        std::pair{ Timing::Place, std::string_view("place") },
        std::pair{ Timing::Bake, std::string_view("bake") },
        std::pair{ Timing::Textures, std::string_view("textures") },
        std::pair{ Timing::Upload, std::string_view("upload") },
        std::pair{ Timing::Trace, std::string_view("trace") },
        std::pair{ Timing::Views, std::string_view("views") },
        std::pair{ Timing::Present, std::string_view("present") },
        std::pair{ Timing::Update, std::string_view("update") },
    } };

    inline constexpr std::size_t sTimingCount = sTimings.mNames.size();

    inline constexpr std::size_t indexOf(const Timing timing)
    {
        return static_cast<std::size_t>(timing);
    }

    /// What one measured frame spent on the host, by phase — an array over `Timing`, because that
    /// is what `FrameSamples` holds and what it becomes, and a signature of six doubles in a row is
    /// six chances to hand them over in the wrong order. In the core rather than the bench,
    /// because `SceneUploader` writes `Bake`, `Textures` and `Upload`. `Timing::Frame` and
    /// `Timing::Wait` reach `FrameSamples` by their own routes and are left at nought here.
    struct FrameSpend
    {
        std::array<double, sTimingCount> mMs{};

        double& at(const Timing timing) { return mMs[indexOf(timing)]; }
        double at(const Timing timing) const { return mMs[indexOf(timing)]; }
    };
}
