#ifndef OPENMW_MWRENDER_RTX_BENCH_H
#define OPENMW_MWRENDER_RTX_BENCH_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

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
        /// the one `result.mWaitMs` cannot see.
        void frame(const Rtx::FrameResult& result, double frameMs);

    private:
        void report() const;
        std::string describeRun() const;
        std::string describeWarmup() const;

        /// Flies the player forwards by what `mSpeed` comes to over `frameMs`, and counts the cell
        /// boundary it crossed since the last call. Does nothing where the spec named no speed, or
        /// where there is no player to move yet — and a bench that stands still crosses nothing, so
        /// the count belongs here rather than beside the rows.
        ///
        /// **Off the frame's own length and not a fixed step**, because the game's world moves on
        /// the wall clock: a player crossing a boundary crosses it after the same distance whatever
        /// the frame rate, which is the thing being measured. The harness steps by frame index
        /// instead, for a reason `BenchRequest::mSeconds` gives, and the two are not the same run.
        void fly(double frameMs);

        // Frames or seconds, whichever the spec named; the other is zero.
        std::uint32_t mWanted = 0;
        double mWantedSeconds = 0.0;
        std::uint32_t mWarmup = 0;
        double mWarmupSeconds = 0.0;

        /// World units a second, or zero for a bench that stands still.
        float mSpeed = 0.0f;

        /// The height the route holds, taken where it starts. `fly` says why it is held at all.
        std::optional<float> mHeight;

        std::uint32_t mSeen = 0;
        double mWarmedMs = 0.0;
        double mMeasuredMs = 0.0;
        bool mDone = false;

        /// The cell the last flown frame was drawn in, so a change of it is a boundary crossed.
        /// Compared as an address and never read, which is all an identity needs.
        const void* mCell = nullptr;

        /// How many boundaries the run crossed and what the worst of those frames cost.
        ///
        /// **A count and the worst, for the reason `BenchPlace` gives**: a run crosses a handful,
        /// percentiles over a handful say nothing, and the frame a player feels is the worst one.
        std::uint32_t mCrossings = 0;
        double mCrossWorstMs = 0.0;

        // Out of line so this header names no container, and reserved once so the run itself does
        // not allocate — a bench that stutters where it measures is measuring its own stutter.
        struct Held;
        std::unique_ptr<Held> mHeld;
#else
        /// The shape with nothing in it, so the frame path needs no conditional of its own.
        void frame(const Rtx::FrameResult&, double) {}
#endif
    };
}

#endif
