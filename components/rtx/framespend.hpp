#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "namedenum.hpp"

namespace Rtx
{
    /// Which of a measured frame's figures a row holds.
    ///
    /// **`Wait` is the CPU standing still for the device, `Walk` is the world being mirrored, and
    /// `Place` is the renderer being told what moved.** What is left of `Frame` is the frame's own
    /// record. Lumping them would hide which of them a place is slow because of; a wait near the
    /// frame is a device that cannot keep up, and a wait near nought is a CPU that cannot.
    ///
    /// **`Finish` is the whole of collecting the frame behind, and `Wait` is its largest share.**
    /// The fence is what `Wait` measures; what `Finish` adds is everything the ring does once the
    /// fence has passed — reading the device's counters and its timestamps, and destroying what
    /// that frame was the last to read. The two are apart because one is the device being slow and
    /// the other is this renderer being slow, and a frame can be either.
    ///
    /// **Four more of them are shares of another row rather than of the frame**, and they follow
    /// the row they are inside. `Fold` is what the walk spent folding the geometry which arrived on
    /// it, which is the active cells' own geometry the cell ring did not read first.
    /// `Bake`, `Textures` and `Upload` are the three halves of `Place` that a spike could be in —
    /// the ground the composite queue handed back, the arrived textures being opened and described,
    /// and what the backend was then told. What is left of `Place` is the lists it reads either
    /// side of them.
    ///
    /// **`Trace` and `Present` are the other two calls the frame makes into the backend** — the
    /// record and its submit, and the picture reaching the surface with the interface over it.
    /// **`Update` is the rest of the loop and it is the game's**: the world stepping and the cells
    /// arriving, between one call into the renderer and the next.
    ///
    /// **All four are timed rather than profiled, because a profile cannot read them.** Most of
    /// what a call into the driver costs is inside the driver, which carries no frame pointer to
    /// walk, and a thread asleep is counted by a sampling profiler as nothing at all. Measured on
    /// the island route, perf reads the trace call at nine microseconds a frame and the wall clock
    /// reads it at a hundred and eighty.
    ///
    /// **Together they close the frame.** `Frame` less `Finish`, `Walk`, `Place`, `Views`, `Trace`,
    /// `Present` and `Update` is under 0.05 ms at every place that stands still: what is left is the
    /// window's extent being handed over, the sweep, and the frame's own record. A row that does
    /// not close is a stretch nobody has named, which is what these were added to find.
    ///
    /// **Why `Place` needed splitting at all**: on the island route it is 0.18 ms at the median and
    /// 62 at the worst, and a profile cannot say which half — an arrival frame's time is in the
    /// driver, and the driver carries no frame pointer for perf to walk.
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

    /// What a report heads each row with, and — with `Ms` after it — what the JSON names it.
    ///
    /// **One table, and the count and the walk are derived from it.** They were three lists of
    /// twelve kept level by hand, so a row added to the enum reached a report only if somebody
    /// wrote it out twice more. `Rtx::NamedEnum` says at length why an enum states its spellings
    /// once.
    inline constexpr NamedEnum<Timing, 13> sTimings{ { {
        { Timing::Frame, "frame" },
        { Timing::Finish, "finish" },
        { Timing::Wait, "wait" },
        { Timing::Walk, "walk" },
        { Timing::Fold, "fold" },
        { Timing::Place, "place" },
        { Timing::Bake, "bake" },
        { Timing::Textures, "textures" },
        { Timing::Upload, "upload" },
        { Timing::Trace, "trace" },
        { Timing::Views, "views" },
        { Timing::Present, "present" },
        { Timing::Update, "update" },
    } } };

    inline constexpr std::size_t sTimingCount = sTimings.mNames.size();

    inline constexpr std::size_t indexOf(const Timing timing)
    {
        return static_cast<std::size_t>(timing);
    }

    /// What one measured frame spent on the host, by phase.
    ///
    /// **One bag, because the list only ever grows.** Each of these arrives at the same call from a
    /// different place, and a signature that named them one by one was six parameters of one type
    /// in a row — which is six chances to hand them over in the wrong order and no way to be caught
    /// at it.
    ///
    /// **An array over `Timing` and not ten named members**, because that is what `FrameSamples`
    /// holds and what it becomes: named, the two shapes were joined by a hand copy of one line per
    /// figure, so a row added to the enum took three edits before a producer could state it.
    ///
    /// **In the core rather than the bench**, because the hand-over is what fills three of its
    /// rows: `SceneUploader` writes `Bake`, `Textures` and `Upload` into the one a frame hands it.
    ///
    /// `Timing::Frame` and `Timing::Wait` are left at nought here. The first is the whole frame and
    /// the second is what the device answered for, and each reaches `FrameSamples` by its own route.
    struct FrameSpend
    {
        std::array<double, sTimingCount> mMs{};

        double& at(const Timing timing) { return mMs[indexOf(timing)]; }
        double at(const Timing timing) const { return mMs[indexOf(timing)]; }
    };
}
