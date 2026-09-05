#pragma once

#include <cstdint>
#include <memory>
#include <optional>

// Last, and conditional, because nothing outside the run below names either of them.
#ifdef OPENMW_RTX_BENCH
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchspec.hpp>
#endif

namespace Rtx
{
    struct FrameResult;
}

namespace MWRender
{
    /// Times a run of frames inside the running game and reports what `openmw-rtxtool bench` does.
    ///
    /// **Why the game and not the harness.** The harness stages a world once and re-walks only its
    /// actors, so it never pays for the whole-graph walk, the sweep, or a cell arriving — the three
    /// things that actually cost the game a frame. Every renderer defect this fork has found in the
    /// last stretch was invisible to `bench` and obvious the moment the game was measured.
    ///
    /// **It changes nothing outside this directory.** No command line, no engine loop, no rendering
    /// manager: it reads one environment variable, is fed each frame by `Tracer`, and ends the run
    /// through `StateManager::requestQuit` the way the player's quit key does. Where
    /// `OPENMW_RTX_BENCH` is not defined the class below has no members and no body, so the frame
    /// path costs nothing and a shipping build contains none of it.
    ///
    ///     OPENMW_RTX_BENCH=600           measure 600 frames, then report and quit
    ///     OPENMW_RTX_BENCH=10s           measure ten seconds of them instead
    ///     OPENMW_RTX_BENCH=10s:2s        the same, after two seconds warming up
    ///     OPENMW_RTX_BENCH=10s:2s@12000  and fly forwards at 12000 units a second while measuring
    ///
    /// Where to stand is a savegame's business: `--load-savegame` puts the player, the camera and
    /// the world back exactly, which no pair of coordinates can. Which way to fly is the same
    /// business — the route is the way the save left the player facing, so a route is a speed and
    /// nothing else.
    ///
    /// **What a speed is for: the cost of a cell arriving, in the game.** A standing bench measures
    /// a frame; the ring read off the disk, the models instanced out of it, and the sweep that
    /// follows the cells left behind only happen to a player who goes somewhere. `openmw-rtxtool
    /// bench --suite=streaming` measures that in the harness, and the harness has no `CellPreloader`
    /// — so its crossing pays what the game's does not, and the two numbers only mean something
    /// beside each other.
    class Bench
    {
    public:
#ifdef OPENMW_RTX_BENCH
        /// Reads `OPENMW_RTX_BENCH`. Inert, and silent, where it is unset or unreadable.
        Bench();
        ~Bench();

        Bench(const Bench&) = delete;
        Bench& operator=(const Bench&) = delete;

        /// Takes one traced frame. Reports and asks the game to quit once the run is done.
        ///
        /// `frameMs` is the whole frame and not the wait: measured from one call to the next, so it
        /// carries everything the game does between them — which is the number a player feels and
        /// the one `result.mWaitMs` cannot see. `walkMs` and `placeMs` are the two shares of it this
        /// fork owns — mirroring the world, and handing the result to the device — and they are what
        /// says whether a crossing frame is this renderer's or the engine's underneath it.
        ///
        /// `rebuilt` where the hand-over could not append and built the scene again from nothing,
        /// which `Crossings` says is the most useful thing a route reports.
        void frame(const Rtx::FrameResult& result, double frameMs, double walkMs, double placeMs, bool rebuilt);

    private:
        /// Not `const`: `Rtx::summarise` sorts each row of samples where it lies.
        void report();

        /// Flies the player forwards by what `mSpeed` comes to over `frameMs`, and counts the cell
        /// boundary it crossed since the last call — and whether that crossing had to be rebuilt.
        /// Does nothing where the spec named no speed, or where there is no player to move yet —
        /// and a bench that stands still crosses nothing, so the count belongs here rather than
        /// beside the rows.
        ///
        /// **Off the frame's own length and not a fixed step**, because a played session's world
        /// moves on the wall clock: a player crossing a boundary crosses it after the same distance
        /// whatever the frame rate, which is the thing being measured. A run that has to be
        /// comparable with itself states its step instead — `[RTX] fixed step` — and the two are
        /// not the same run.
        void fly(double frameMs, bool rebuilt);

        /// How long the run is, how much of it warms up, and how fast it flies.
        Rtx::BenchSpec mSpec;

        /// The height the route holds, taken where it starts. `fly` says why it is held at all.
        std::optional<float> mHeight;

        std::uint32_t mSeen = 0;
        double mMeasuredMs = 0.0;
        bool mDone = false;

        /// The cell the last flown frame was drawn in, so a change of it is a boundary crossed.
        /// Compared as an address and never read, which is all an identity needs.
        const void* mCell = nullptr;

        /// How many boundaries the run crossed, how many of those could not be appended to, and
        /// what the worst of those frames cost. `Rtx::Crossings` says why a count and a worst.
        Rtx::Crossings mCrossings;

        // Out of line so this header names no container, and reserved once so the run itself does
        // not allocate — a bench that stutters where it measures is measuring its own stutter.
        struct Held;
        std::unique_ptr<Held> mHeld;
#else
        /// The shape with nothing in it, so the frame path needs no conditional of its own.
        void frame(const Rtx::FrameResult&, double, double, double, bool) {}
#endif
    };
}
