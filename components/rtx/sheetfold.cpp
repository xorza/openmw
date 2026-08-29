#include "sheetfold.hpp"

#include <algorithm>

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

        bool before(const osg::Vec3f (&a)[3], const osg::Vec3f (&b)[3])
        {
            for (int i = 0; i < 3; ++i)
            {
                if (before(a[i], b[i]))
                    return true;
                if (before(b[i], a[i]))
                    return false;
            }
            return false;
        }

        bool same(const osg::Vec3f (&a)[3], const osg::Vec3f (&b)[3])
        {
            return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
        }

        /// The corners rotated so the least comes first: the winding survives and where the file
        /// happened to start the triangle does not, which is what lets two spellings of one
        /// triangle compare equal.
        void canonical(const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c, osg::Vec3f (&out)[3])
        {
            const osg::Vec3f* corners[3] = { &a, &b, &c };
            int least = 0;
            for (int i = 1; i < 3; ++i)
                if (before(*corners[i], *corners[least]))
                    least = i;

            out[0] = *corners[least];
            out[1] = *corners[(least + 1) % 3];
            out[2] = *corners[(least + 2) % 3];
        }
    }

    bool SheetFold::fold(std::span<const osg::Vec3f> positions, std::vector<std::uint32_t>& indices)
    {
        const std::size_t count = indices.size() / 3;
        if (count == 0)
            return false;

        mKeys.clear();
        mKeys.reserve(count);
        for (std::size_t t = 0; t < count; ++t)
        {
            Keyed key{ .mTriangle = static_cast<std::uint32_t>(t) };
            canonical(
                positions[indices[3 * t]], positions[indices[3 * t + 1]], positions[indices[3 * t + 2]], key.mCorner);
            mKeys.push_back(key);
        }

        // By corners, then by triangle, so a run of equal corners yields the earlier triangle first
        // and the copy that is kept is the one the file wrote first.
        std::sort(mKeys.begin(), mKeys.end(), [](const Keyed& a, const Keyed& b) {
            if (before(a.mCorner, b.mCorner))
                return true;
            if (before(b.mCorner, a.mCorner))
                return false;
            return a.mTriangle < b.mTriangle;
        });

        mFates.assign(count, Fate::Alone);
        for (std::size_t t = 0; t < count; ++t)
        {
            if (mFates[t] != Fate::Alone)
                continue;

            Keyed reversed{};
            canonical(positions[indices[3 * t]], positions[indices[3 * t + 2]], positions[indices[3 * t + 1]],
                reversed.mCorner);

            auto it = std::lower_bound(mKeys.begin(), mKeys.end(), reversed,
                [](const Keyed& a, const Keyed& b) { return before(a.mCorner, b.mCorner); });
            for (; it != mKeys.end() && same(it->mCorner, reversed.mCorner); ++it)
            {
                // Itself, for a degenerate triangle whose reverse is its own spelling.
                if (it->mTriangle == t || mFates[it->mTriangle] != Fate::Alone)
                    continue;

                mFates[t] = Fate::Kept;
                mFates[it->mTriangle] = Fate::Dropped;
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
