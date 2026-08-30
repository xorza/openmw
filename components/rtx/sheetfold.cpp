#include "sheetfold.hpp"

#include <algorithm>
#include <bit>

namespace Rtx
{
    namespace
    {
        bool before(const osg::Vec3f& a, const osg::Vec3f& b)
        {
            if (a.x() != b.x())
                return a.x() < b.x();
            if (a.y() != b.y())
                return a.y() < b.y();
            return a.z() < b.z();
        }
    }

    bool SheetFold::Corners::operator==(const Corners& other) const
    {
        return mCorner[0] == other.mCorner[0] && mCorner[1] == other.mCorner[1] && mCorner[2] == other.mCorner[2];
    }

    std::size_t SheetFold::Corners::hash() const
    {
        // **The bits, mixed here rather than through `Misc::hashCombine`.** That reaches
        // `std::hash<float>`, which is `_Hash_bytes` — a byte-wise murmur over four bytes — and
        // eighteen of those per triangle put it at 3.7 % of a streaming route's whole CPU, more
        // than the fold around it. FNV over the bits and one final mix cost nine multiplies.
        //
        // **A zero is normalised first**, because -0 and 0 compare equal above and a spelling that
        // hashed differently for the two would never find its twin.
        std::uint64_t seed = 0xcbf29ce484222325ull;
        for (const osg::Vec3f& corner : mCorner)
        {
            for (const float value : { corner.x(), corner.y(), corner.z() })
            {
                seed ^= std::bit_cast<std::uint32_t>(value == 0.0f ? 0.0f : value);
                seed *= 0x100000001b3ull;
            }
        }

        // FNV moves its high bits far more than its low ones, and the table masks the low ones.
        seed ^= seed >> 29;
        seed *= 0xbf58476d1ce4e5b9ull;
        seed ^= seed >> 32;

        return static_cast<std::size_t>(seed);
    }

    SheetFold::Corners SheetFold::canonical(const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c)
    {
        const osg::Vec3f* corners[3] = { &a, &b, &c };
        int least = 0;
        for (int i = 1; i < 3; ++i)
            if (before(*corners[i], *corners[least]))
                least = i;

        return Corners{ { *corners[least], *corners[(least + 1) % 3], *corners[(least + 2) % 3] } };
    }

    bool SheetFold::fold(std::span<const osg::Vec3f> positions, std::vector<std::uint32_t>& indices)
    {
        const std::size_t count = indices.size() / 3;
        if (count == 0)
            return false;

        // Grown to the largest mesh yet folded and never shrunk, so a run of them stops resizing:
        // every entry below `count` is written before it is read.
        if (mSpelling.size() < count)
        {
            mSpelling.resize(count);
            mHashes.resize(count);
        }

        for (std::size_t t = 0; t < count; ++t)
        {
            mSpelling[t]
                = canonical(positions[indices[3 * t]], positions[indices[3 * t + 1]], positions[indices[3 * t + 2]]);
            mHashes[t] = mSpelling[t].hash();
        }

        const std::size_t slots = std::bit_ceil(std::max<std::size_t>(count * 2, 16));
        const std::size_t mask = slots - 1;
        mTable.assign(slots, sNoTriangle);
        mNext.assign(count, sNoTriangle);

        // **From the last triangle back, so each chain runs forwards.** The pairing below takes the
        // first triangle of a spelling that is still unpaired, and which one that is decides which
        // copy of a doubled card survives: the one the file wrote first, as it was before anything
        // was folded.
        for (std::size_t t = count; t-- > 0;)
        {
            std::size_t at = mHashes[t] & mask;
            for (;; at = (at + 1) & mask)
            {
                const std::uint32_t head = mTable[at];
                if (head == sNoTriangle)
                    break;

                if (mHashes[head] == mHashes[t] && mSpelling[head] == mSpelling[t])
                {
                    mNext[t] = head;
                    break;
                }
            }

            mTable[at] = static_cast<std::uint32_t>(t);
        }

        mFates.assign(count, Fate::Alone);
        for (std::size_t t = 0; t < count; ++t)
        {
            if (mFates[t] != Fate::Alone)
                continue;

            const Corners reversed
                = canonical(positions[indices[3 * t]], positions[indices[3 * t + 2]], positions[indices[3 * t + 1]]);
            const std::size_t hash = reversed.hash();

            std::uint32_t head = sNoTriangle;
            for (std::size_t at = hash & mask;; at = (at + 1) & mask)
            {
                const std::uint32_t candidate = mTable[at];
                if (candidate == sNoTriangle)
                    break;

                if (mHashes[candidate] == hash && mSpelling[candidate] == reversed)
                {
                    head = candidate;
                    break;
                }
            }

            for (std::uint32_t other = head; other != sNoTriangle; other = mNext[other])
            {
                // Itself, for a degenerate triangle whose reverse is its own spelling.
                if (other == t || mFates[other] != Fate::Alone)
                    continue;

                mFates[t] = Fate::Kept;
                mFates[other] = Fate::Dropped;
                break;
            }
        }

        std::size_t kept = 0;
        bool sheet = true;
        for (std::size_t t = 0; t < count; ++t)
        {
            if (mFates[t] == Fate::Dropped)
                continue;
            if (mFates[t] == Fate::Alone)
                sheet = false;

            if (kept != t)
                std::copy_n(indices.begin() + static_cast<std::ptrdiff_t>(3 * t), 3,
                    indices.begin() + static_cast<std::ptrdiff_t>(3 * kept));
            ++kept;
        }
        indices.resize(kept * 3);

        return sheet;
    }
}
