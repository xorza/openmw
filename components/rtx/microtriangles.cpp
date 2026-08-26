#include "microtriangles.hpp"

#include <algorithm>

namespace Rtx
{
    namespace
    {
        /// Spreads sixteen bits out into every other bit of a word.
        std::uint32_t interleave(std::uint32_t bits)
        {
            bits = (bits | (bits << 8u)) & 0x00ff00ffu;
            bits = (bits | (bits << 4u)) & 0x0f0f0f0fu;
            bits = (bits | (bits << 2u)) & 0x33333333u;
            bits = (bits | (bits << 1u)) & 0x55555555u;

            return bits;
        }

        osg::Vec2f latticeCorner(std::uint32_t u, std::uint32_t v, float step)
        {
            return osg::Vec2f(static_cast<float>(u) * step, static_cast<float>(v) * step);
        }

        /// The middle of a microtriangle, which is what asks the curve where it belongs.
        ///
        /// A corner sits on the boundary between as many as six microtriangles and would be
        /// quantized into whichever of them the arithmetic happened to round towards; the middle is
        /// a third of a cell clear of every edge, which no rounding at this scale can cross.
        osg::Vec2f middleOf(const Microtriangle& micro)
        {
            return (micro.mCorners[0] + micro.mCorners[1] + micro.mCorners[2]) / 3.0f;
        }
    }

    std::uint32_t microtriangleIndexAt(float u, float v, std::uint32_t level)
    {
        const std::uint32_t across = 1u << level;

        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);

        const float fu = u * static_cast<float>(across);
        const float fv = v * static_cast<float>(across);

        auto iu = static_cast<std::uint32_t>(fu);
        auto iv = static_cast<std::uint32_t>(fv);

        const float uf = fu - static_cast<float>(iu);
        const float vf = fv - static_cast<float>(iv);

        if (iu >= across)
            iu = across - 1u;
        if (iv >= across)
            iv = across - 1u;

        const std::uint32_t iuv = iu + iv;
        if (iuv >= across)
            iu -= iuv - across + 1u;

        std::uint32_t iw = ~(iu + iv);
        if (uf + vf >= 1.0f && iuv < across - 1u)
            --iw;

        std::uint32_t b0 = ~(iu ^ iw) & (across - 1u);
        const std::uint32_t t = (iu ^ iv) & b0;

        std::uint32_t f = t;
        f ^= f >> 1u;
        f ^= f >> 2u;
        f ^= f >> 4u;
        f ^= f >> 8u;
        const std::uint32_t b1 = ((f ^ iu) & ~b0) | t;

        return interleave(b0) | (interleave(b1) << 1u);
    }

    void subdivideTriangle(std::uint32_t level, std::vector<Microtriangle>& into)
    {
        const std::uint32_t across = 1u << level;
        const float step = 1.0f / static_cast<float>(across);

        into.clear();
        into.reserve(std::size_t{ across } * across);

        // The lattice, row by row: a cell that points the way the triangle does, and between each
        // pair of them one that points the other way. A row of `n` upright cells carries `n - 1`
        // flipped ones, which is what makes the rows sum to `4^level` rather than to twice it.
        for (std::uint32_t v = 0; v < across; ++v)
            for (std::uint32_t u = 0; u + v < across; ++u)
            {
                Microtriangle upright;
                upright.mCorners[0] = latticeCorner(u, v, step);
                upright.mCorners[1] = latticeCorner(u + 1, v, step);
                upright.mCorners[2] = latticeCorner(u, v + 1, step);
                into.push_back(upright);

                if (u + v + 1 < across)
                {
                    Microtriangle flipped;
                    flipped.mCorners[0] = latticeCorner(u + 1, v, step);
                    flipped.mCorners[1] = latticeCorner(u, v + 1, step);
                    flipped.mCorners[2] = latticeCorner(u + 1, v + 1, step);
                    into.push_back(flipped);
                }
            }

        for (Microtriangle& micro : into)
        {
            const osg::Vec2f middle = middleOf(micro);
            micro.mIndex = microtriangleIndexAt(middle.x(), middle.y(), level);
        }
    }
}
