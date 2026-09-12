#include "runs.hpp"

#include <algorithm>
#include <cassert>

namespace Rtx
{
    Run RunAllocator::place(const Run& hole, std::uint32_t count) const
    {
        std::uint32_t at = hole.mOffset;

        // Moved to the next boundary rather than rejected, because the hole may be much longer than
        // the tail of the block it starts in. `count` never exceeds a block, so one step is enough.
        if (mBlock != 0 && at / mBlock != (at + count - 1) / mBlock)
            at = (at / mBlock + 1) * mBlock;

        if (at + count > hole.getEnd())
            return Run{};

        return Run{ .mOffset = at, .mCount = count };
    }

    Run RunAllocator::allocate(std::uint32_t count)
    {
        assert(count > 0);
        assert(mBlock == 0 || count <= mBlock);

        auto best = mFree.end();
        Run taken;

        for (auto hole = mFree.begin(); hole != mFree.end(); ++hole)
        {
            const Run here = place(*hole, count);
            if (here.empty())
                continue;

            // **The hole's own size, because that is what the waste is.** Whatever a run's
            // alignment skips at the front of a hole and whatever is left behind it come to
            // `hole->mCount - count` between them, so the smallest hole that fits wastes the least
            // whatever the block size does to where the run lands.
            if (best == mFree.end() || hole->mCount < best->mCount)
            {
                best = hole;
                taken = here;
            }
        }

        if (best != mFree.end())
        {
            // Up to two leftovers, because a run pushed to a boundary leaves the tail of the block
            // it skipped as well as whatever follows it.
            const Run before{ .mOffset = best->mOffset, .mCount = taken.mOffset - best->mOffset };
            const Run after{ .mOffset = taken.getEnd(), .mCount = best->getEnd() - taken.getEnd() };

            if (before.empty())
                best = mFree.erase(best);
            else
            {
                *best = before;
                ++best;
            }

            if (!after.empty())
                mFree.insert(best, after);

            return taken;
        }

        std::uint32_t at = mEnd;
        if (mBlock != 0 && at / mBlock != (at + count - 1) / mBlock)
        {
            const std::uint32_t boundary = (at / mBlock + 1) * mBlock;

            // The tail is a hole and not waste: something shorter will fit in it later.
            mFree.push_back(Run{ .mOffset = at, .mCount = boundary - at });
            at = boundary;
        }

        mEnd = at + count;
        return Run{ .mOffset = at, .mCount = count };
    }

    void RunAllocator::release(Run span)
    {
        if (span.empty())
            return;

        assert(span.getEnd() <= mEnd);

        const auto after = std::lower_bound(mFree.begin(), mFree.end(), span.mOffset,
            [](const Run& hole, std::uint32_t offset) { return hole.mOffset < offset; });

        assert(after == mFree.end() || span.getEnd() <= after->mOffset);
        assert(after == mFree.begin() || (after - 1)->getEnd() <= span.mOffset);

        auto merged = mFree.insert(after, span);

        if (merged + 1 != mFree.end() && merged->getEnd() == (merged + 1)->mOffset)
        {
            merged->mCount += (merged + 1)->mCount;
            mFree.erase(merged + 1);
        }

        if (merged != mFree.begin() && (merged - 1)->getEnd() == merged->mOffset)
        {
            --merged;
            merged->mCount += (merged + 1)->mCount;
            mFree.erase(merged + 1);
        }

        // **A hole at the very end is not a hole, it is room never used.** Giving it back keeps the
        // buffer as short as what is in it, so a region walked away from and never returned to stops
        // costing anything at all rather than costing a hole for ever.
        if (merged->getEnd() == mEnd)
        {
            mEnd = merged->mOffset;
            mFree.erase(merged);
        }
    }

    void RunAllocator::clear()
    {
        mFree.clear();
        mEnd = 0;
    }

    std::uint32_t RunAllocator::getFree() const
    {
        std::uint32_t free = 0;
        for (const Run& hole : mFree)
            free += hole.mCount;

        return free;
    }
}
