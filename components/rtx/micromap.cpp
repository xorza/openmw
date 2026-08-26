#include "micromap.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

#include "microtriangles.hpp"

namespace Rtx
{
    namespace
    {
        /// How many bytes one triangle's states take at a level: two bits each, and never fewer
        /// than the one byte a level-nought triangle's single state still has to sit in.
        std::uint32_t stateBytesFor(std::uint32_t level)
        {
            return std::max(1u, (1u << (2u * level)) / 4u);
        }

        /// The texture coordinate at a point of a triangle, in the barycentrics a hit reports.
        osg::Vec2f uvAt(const osg::Vec2f (&uv)[3], const osg::Vec2f& bary)
        {
            return uv[0] * (1.0f - bary.x() - bary.y()) + uv[1] * bary.x() + uv[2] * bary.y();
        }

        /// The patch of mask a triangle of texture coordinates can reach.
        ///
        /// The corners are enough because the map from barycentrics to texture coordinates is
        /// affine: nothing inside a triangle lies outside the box its three corners make.
        UvBox boxOf(const osg::Vec2f& first, const osg::Vec2f& second, const osg::Vec2f& third)
        {
            return UvBox{
                std::min({ first.x(), second.x(), third.x() }),
                std::min({ first.y(), second.y(), third.y() }),
                std::max({ first.x(), second.x(), third.x() }),
                std::max({ first.y(), second.y(), third.y() }),
            };
        }

        /// How much of the mask a triangle covers, in the finest level's texels.
        float texelsUnder(const osg::Vec2f (&uv)[3], float grid)
        {
            const float area = 0.5f
                * std::abs((uv[1].x() - uv[0].x()) * (uv[2].y() - uv[0].y())
                    - (uv[2].x() - uv[0].x()) * (uv[1].y() - uv[0].y()));

            return area * grid;
        }

        /// How finely to cut a triangle covering `texels` of the mask.
        ///
        /// Rounded up rather than to nearest, so the target is a ceiling on what a microtriangle
        /// covers and not an average it sits either side of. A triangle already under the target,
        /// and one whose coordinates are degenerate or not a number, both come back at level
        /// nought — which is the caller's signal to keep the answer it already has.
        std::uint32_t chosenLevel(float texels, std::uint32_t maxLevel)
        {
            if (!(texels > Micromap::sTexelsPerMicrotriangle))
                return 0;

            // Four microtriangles a level, so the level is half the base-two logarithm.
            const float steps = 0.5f * std::log2(texels / Micromap::sTexelsPerMicrotriangle);

            return std::min(static_cast<std::uint32_t>(std::ceil(steps)), maxLevel);
        }
    }

    Micromap::Micromap(std::span<const osg::Vec2f> texCoords, std::span<const std::uint32_t> indices,
        const osg::Vec4f& transform, const AlphaBounds& bounds, std::uint32_t maxLevel)
    {
        assert(indices.size() % 3 == 0);

        // A mesh with no coordinates has no mask to place, and a mask that could not be decoded has
        // nothing to say. Both leave this empty, which the caller reads as *build none*.
        if (texCoords.empty() || indices.empty() || bounds.isEmpty())
            return;

        const std::uint32_t ceiling = std::min(maxLevel, sSubdivisionCeiling);
        const auto triangles = static_cast<std::uint32_t>(indices.size() / 3);
        const auto grid = static_cast<float>(bounds.getWidth()) * static_cast<float>(bounds.getHeight());

        mTriangles.reserve(triangles);

        // A byte a triangle is the floor, since that is exactly what a resolved one takes — so a
        // mesh of resolved triangles goes to the allocator once, and every other mesh starts past
        // the first few doublings.
        mData.reserve(triangles);

        std::array<std::uint32_t, sSubdivisionCeiling + 1> perLevel{};

        // **One lattice per level and not one at a time**, because a mesh's triangles are not sorted
        // by how much of the mask they cover: a level kept and thrown away as the cut moved between
        // neighbouring triangles would subdivide afresh for nearly every one of them.
        std::array<std::vector<Microtriangle>, sSubdivisionCeiling + 1> lattices;
        std::vector<Opacity> states;

        for (std::uint32_t triangle = 0; triangle < triangles; ++triangle)
        {
            // **The material's transform applied the way `sampleDiffuse` applies it**, because a
            // micromap is only correct for as long as this and the cutout shader agree about which
            // texel a hit reads. The same holds of `uvAt` below and the shader's `interpolate`: the
            // arithmetic is theirs, not a restatement of it.
            osg::Vec2f uv[3];
            for (std::uint32_t corner = 0; corner < 3; ++corner)
            {
                const osg::Vec2f& source = texCoords[indices[triangle * 3 + corner]];
                uv[corner] = osg::Vec2f(
                    source.x() * transform.x() + transform.z(), source.y() * transform.y() + transform.w());
            }

            states.clear();

            // **The whole triangle first, and it is the ordinary answer.** Barely any of Morrowind's
            // alpha-blended surfaces is a leaf: a banner, a grate or a pane is opaque across every
            // texel it names, and the card a canopy is painted on is empty across most of them. One
            // query settles those, and nothing below it runs.
            const Opacity whole = bounds.classify(boxOf(uv[0], uv[1], uv[2]));

            std::uint32_t level = 0;
            if (whole == Opacity::Unknown)
                level = chosenLevel(texelsUnder(uv, grid), ceiling);

            // Level nought is both of the cases that need no subdivision: a triangle the mask agrees
            // about, and one small enough that its single cell would ask the very same box again.
            if (level == 0)
                states.push_back(whole);
            else
            {
                std::vector<Microtriangle>& lattice = lattices[level];
                if (lattice.empty())
                    subdivideTriangle(level, lattice);

                states.assign(lattice.size(), Opacity::Unknown);

                Opacity common = Opacity::Unknown;
                bool uniform = true;
                for (std::size_t at = 0; at < lattice.size(); ++at)
                {
                    const Microtriangle& micro = lattice[at];
                    const Opacity state = bounds.classify(
                        boxOf(uvAt(uv, micro.mCorners[0]), uvAt(uv, micro.mCorners[1]), uvAt(uv, micro.mCorners[2])));

                    states[micro.mIndex] = state;
                    if (at == 0)
                        common = state;
                    else
                        uniform = uniform && state == common;
                }

                // **A triangle whose microtriangles all agree collapses, and the box above did not
                // catch it.** A triangle's own box takes in the half of it the triangle is not, so a
                // leaf card cut corner to corner reads as straddling the mask while every piece of it
                // is clear of it.
                if (uniform)
                {
                    level = 0;
                    states.assign(1, common);
                }
            }

            const std::uint32_t bytes = stateBytesFor(level);
            const auto offset = static_cast<std::uint32_t>(mData.size());
            mData.resize(mData.size() + bytes, std::byte{ 0 });

            for (std::size_t index = 0; index < states.size(); ++index)
            {
                const auto packed
                    = static_cast<std::uint8_t>(static_cast<std::uint8_t>(states[index]) << (2u * (index % 4u)));
                mData[offset + index / 4] |= std::byte{ packed };
            }

            mTriangles.push_back(MicromapTriangle{ offset, static_cast<std::uint16_t>(level) });
            ++perLevel[level];

            const double share = 1.0 / static_cast<double>(states.size());
            for (const Opacity state : states)
            {
                if (state == Opacity::Opaque)
                    mTally.mOpaque += share;
                else if (state == Opacity::Transparent)
                    mTally.mTransparent += share;
                else
                    mTally.mUnknown += share;
            }
        }

        for (std::uint32_t level = 0; level < perLevel.size(); ++level)
            if (perLevel[level] != 0)
                mUsage.push_back(MicromapUsage{ perLevel[level], level });
    }

    Opacity Micromap::at(std::uint32_t triangle, std::uint32_t index) const
    {
        const MicromapTriangle& which = mTriangles[triangle];
        assert(index < (1u << (2u * which.mSubdivisionLevel)));

        const auto packed = static_cast<std::uint8_t>(mData[which.mDataOffset + index / 4]);

        return static_cast<Opacity>((packed >> (2u * (index % 4u))) & 0x3u);
    }
}
