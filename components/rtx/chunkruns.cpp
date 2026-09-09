#include "chunkruns.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <utility>

namespace Rtx
{
    std::size_t ChunkRuns::NameHash::operator()(const Terrain::ChunkName& name) const
    {
        // Mixed rather than combined by addition, because a quad tree hands out centres that differ
        // in their low bits alone.
        std::size_t hash = std::hash<float>{}(name.mCentre.x());
        hash = hash * 1000003 + std::hash<float>{}(name.mCentre.y());
        hash = hash * 1000003 + std::hash<float>{}(name.mSize);
        hash = hash * 1000003 + name.mLodFlags;

        return hash * 1000003 + (name.mActiveGrid ? 1u : 0u);
    }

    void ChunkRuns::Round::clear()
    {
        mSteps.clear();
        mRuns.clear();
    }

    void ChunkRuns::beginWalk(const std::uint64_t epoch)
    {
        assert(!mOpened && "a walk begun with a chunk still open");

        // **Once a round and not once a walk.** The game walks its precipitation beside its world,
        // and both carry the epoch the sweep is counting; a swap per walk would hand the second one
        // what the first had just written and call every chunk of it new.
        if (epoch == mEpoch)
            return;

        mEpoch = epoch;

        std::swap(mLast, mNow);
        mNow.clear();
    }

    void ChunkRuns::open(const Terrain::ChunkName& name)
    {
        assert(!mOpened && "a chunk opened inside another");

        mOpen = name;
        mOpenAt = static_cast<std::uint32_t>(mNow.mSteps.size());
        mOpened = true;
        mWalkReplayable = true;

        mRecorded = {};
        mCanReplay = false;
        if (const auto found = mLast.mRuns.find(mOpen); found != mLast.mRuns.end())
        {
            mRecorded = std::span(mLast.mSteps).subspan(found->second.mFirst, found->second.mCount);
            mCanReplay = found->second.mReplayable;
        }
    }

    void ChunkRuns::refuse()
    {
        if (mOpened)
            mWalkReplayable = false;
    }

    void ChunkRuns::add(const ChunkStep& step)
    {
        if (mOpened)
            mNow.mSteps.push_back(step);
    }

    void ChunkRuns::close(const Ended how)
    {
        assert(mOpened && "a chunk closed that was never opened");

        if (how == Ended::Replayed)
        {
            assert(canReplay() && "a chunk replayed from a run that refused one");
            assert(mNow.mSteps.size() == mOpenAt && "a chunk that was replayed also walked");
            mNow.mSteps.insert(mNow.mSteps.end(), mRecorded.begin(), mRecorded.end());
        }
        else
        {
            // **The rule a replay stands on, checked wherever the walk ran anyway.** A chunk met
            // under a name it was met under before must walk to the run it walked to before — the
            // same drawables, in the same order, to the same slots. `Terrain::ObjectPaging` caches a
            // chunk and hands the same geometry back, so a difference here is the cache handing out
            // something the name does not describe, and a replay would mirror the wrong ground.
            assert((mRecorded.empty()
                       || (mRecorded.size() == mNow.mSteps.size() - mOpenAt
                           && std::equal(mRecorded.begin(), mRecorded.end(), mNow.mSteps.begin() + mOpenAt)))
                && "a paged chunk met twice under one name walked to two different runs");
        }

        // **The first hand-over of a name wins the round, and the second is thrown away.** A stop
        // that asks for the graph twice — `Session::wantsSecondWalk` — hands every chunk over on
        // both walks, and the two produce the same run. Kept, the buffer would carry a copy of the
        // whole terrain that no run names and every chunk after it would sit further along.
        const auto recorded = mNow.mRuns.emplace(mOpen,
            Run{
                .mFirst = mOpenAt,
                .mCount = static_cast<std::uint32_t>(mNow.mSteps.size() - mOpenAt),

                // A replayed chunk was not walked, so what it holds is what the walk that recorded
                // it found — and that run was replayable or it would not have been replayed.
                .mReplayable = how == Ended::Replayed || mWalkReplayable,
            });

        if (!recorded.second)
            mNow.mSteps.resize(mOpenAt);

        mOpened = false;
        mRecorded = {};
        mCanReplay = false;
    }

    void ChunkRuns::clear()
    {
        mLast.clear();
        mNow.clear();
        mRecorded = {};
        mCanReplay = false;
        mOpened = false;
    }
}
