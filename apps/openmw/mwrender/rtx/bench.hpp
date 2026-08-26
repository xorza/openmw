#ifndef OPENMW_MWRENDER_RTX_BENCH_H
#define OPENMW_MWRENDER_RTX_BENCH_H

#include <cstdint>
#include <memory>
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
    ///     OPENMW_RTX_BENCH=600        measure 600 frames, then report and quit
    ///     OPENMW_RTX_BENCH=10s        measure ten seconds of them instead
    ///     OPENMW_RTX_BENCH=10s:2s     the same, after two seconds warming up
    ///
    /// Where to stand is a savegame's business: `--load-savegame` puts the player, the camera and
    /// the world back exactly, which no pair of coordinates can.
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
        /// `frameMs` is the whole frame and not the trace: measured from one call to the next, so it
        /// carries everything the game does between them — which is the number a player feels and
        /// the one `result.mTraceMs` cannot see.
        void frame(const Rtx::FrameResult& result, double frameMs);

    private:
        void report() const;
        std::string describeRun() const;
        std::string describeWarmup() const;

        // Frames or seconds, whichever the spec named; the other is zero.
        std::uint32_t mWanted = 0;
        double mWantedSeconds = 0.0;
        std::uint32_t mWarmup = 0;
        double mWarmupSeconds = 0.0;

        std::uint32_t mSeen = 0;
        double mWarmedMs = 0.0;
        double mMeasuredMs = 0.0;
        bool mDone = false;

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
