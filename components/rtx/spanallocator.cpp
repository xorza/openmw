#include "spanallocator.hpp"

#include <algorithm>
#include <cassert>

namespace Rtx
{
    Span SpanAllocator::place(const Span& hole, std::uint32_t count) const
    {
        std::uint32_t at = hole.mOffset;

        // Moved to the next boundary rather than rejected, because the hole may be much longer than
        // the tail of the block it starts in. `count` never exceeds a block, so one step is enough.
        if (mBlock != 0 && at / mBlock != (at + count - 1) / mBlock)
            at = (at / mBlock + 1) * mBlock;

        if (at + count > hole.getEnd())
            return Span{};

        return Span{ .mOffset = at, .mCount = count };
    }

    Span SpanAllocator::allocate(std::uint32_t count)
    {
        assert(count > 0);
        assert(mBlock == 0 || count <= mBlock);

        auto best = mFree.end();
        Span taken;

        for (auto hole = mFree.begin(); hole != mFree.end(); ++hole)
        {
            const Span here = place(*hole, count);
            if (here.empty())
                continue;

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
            const Span before{ .mOffset = best->mOffset, .mCount = taken.mOffset - best->mOffset };
            const Span after{ .mOffset = taken.getEnd(), .mCount = best->getEnd() - taken.getEnd() };

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
            mFree.push_back(Span{ .mOffset = at, .mCount = boundary - at });
            at = boundary;
        }

        mEnd = at + count;
        return Span{ .mOffset = at, .mCount = count };
    }

    void SpanAllocator::release(Span span)
    {
        if (span.empty())
            return;

        assert(span.getEnd() <= mEnd);

        const auto after = std::lower_bound(mFree.begin(), mFree.end(), span.mOffset,
            [](const Span& hole, std::uint32_t offset) { return hole.mOffset < offset; });

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

    void SpanAllocator::clear()
    {
        mFree.clear();
        mEnd = 0;
    }

    std::uint32_t SpanAllocator::getFree() const
    {
        std::uint32_t free = 0;
        for (const Span& hole : mFree)
            free += hole.mCount;

        return free;
    }
}
