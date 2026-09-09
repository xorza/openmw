#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <components/terrain/chunktaker.hpp>

#include "index.hpp"

namespace osg
{
    class Drawable;
    class StateSet;
}

namespace Rtx
{
    /// What one drawable of a paged chunk came to, in the order the walk met it.
    ///
    /// **The three identities a walk stamps, and nothing else.** A replay owes the sweep exactly
    /// what a walk would have told it — that this mesh, this material and this placement were
    /// reached — and everything else a walk does for an unchanged chunk arrives at a value the
    /// scene already holds.
    struct ChunkStep
    {
        /// What each of the three is held under, which is what a replay stamps them by.
        std::size_t mWho = 0;
        const osg::Drawable* mDrawable = nullptr;
        const osg::StateSet* mMaterialKey = nullptr;

        /// What each came to. A replay compares what it stamps against these, so a run that has
        /// gone stale between one round and the next is a walk rather than a wrong mirror.
        Index mMesh = sNoIndex;
        Index mMaterial = sNoIndex;
        Index mPlacement = sNoIndex;

        bool operator==(const ChunkStep&) const = default;
    };

    /// What the last walk of each paged chunk produced, keyed on what names the chunk's contents.
    ///
    /// **Keyed on the name and never on the node.** `Terrain::ChunkName` says why: a chunk's
    /// contents come back on whatever transform `loadRenderingNode` had free, so the address is not
    /// the chunk's identity and the name is. Measured on the island route, 98.5% of the chunks a
    /// frame is handed carry the name they carried last frame, against 85.3% that also carry the
    /// same node. `MirrorTraversal::takeChunk` already folds its identity from the name, so a run
    /// recorded under one transform is true under the next.
    ///
    /// **Two buffers rather than one map of vectors.** A frame reads what the last walk wrote and
    /// writes what the next will read, so the pair is swapped rather than rebuilt — which keeps
    /// every run in one flat buffer and allocates nothing once a place has settled.
    class ChunkRuns
    {
    public:
        /// Starts the walk whose epoch is `epoch`, and makes what the walk before it recorded
        /// readable. Two walks of one epoch — the world and the weather beside it — share a round.
        void beginWalk(std::uint64_t epoch);

        /// Opens a run for `name`. Every `add` until `close` belongs to it.
        void open(const Terrain::ChunkName& name);

        /// What the walk before recorded for the open chunk, or nothing where it met no such chunk.
        std::span<const ChunkStep> getRecorded() const { return mRecorded; }

        /// Whether the recorded run may be stamped in place of a walk.
        ///
        /// **False for a chunk holding anything a step cannot describe** — a light, a particle
        /// system, a drawable that mirrored nothing. A run is what the walk placed; a replay of one
        /// short of what the walk also did would drop whatever it left out.
        bool canReplay() const { return mCanReplay && !mRecorded.empty(); }

        void add(const ChunkStep& step);

        /// Says the open chunk holds something no run describes, so it is never replayed.
        void refuse();

        /// How the open chunk was answered, which decides what closing it means.
        enum class Ended
        {
            /// The walk descended into the chunk and the steps below are what it produced.
            Walked,

            /// The recorded run was stamped instead, so this round added no steps of its own.
            Replayed,
        };

        /// Ends the open run.
        ///
        /// **`Walked` asserts that a chunk met twice under one name walked to one run**, which is
        /// the whole rule a replay stands on. **`Replayed` carries the recorded run forward**, so
        /// the round after this one can replay it as well — a run left behind would have every
        /// second frame walk the chunk it had just been spared.
        void close(Ended how);

        /// Forgets every run, for a caller whose scene no longer describes what they were recorded
        /// against.
        void clear();

    private:
        struct NameHash
        {
            std::size_t operator()(const Terrain::ChunkName& name) const;
        };

        /// Where one chunk's steps sit in the buffer beside the map that names it.
        struct Run
        {
            std::uint32_t mFirst = 0;
            std::uint32_t mCount = 0;
            bool mReplayable = true;
        };

        /// One walk's worth: every chunk's steps end to end, and where each chunk's own begin.
        struct Round
        {
            std::vector<ChunkStep> mSteps;
            std::unordered_map<Terrain::ChunkName, Run, NameHash> mRuns;

            void clear();
        };

        Round mLast;
        Round mNow;

        std::uint64_t mEpoch = 0;

        Terrain::ChunkName mOpen;
        std::uint32_t mOpenAt = 0;
        bool mOpened = false;

        /// Whether the chunk being walked has kept to what a run can describe.
        bool mWalkReplayable = true;

        std::span<const ChunkStep> mRecorded;
        bool mCanReplay = false;
    };
}
