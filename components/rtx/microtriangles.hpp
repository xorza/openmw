#pragma once

#include <cstdint>
#include <vector>

#include <osg/Vec2f>

namespace Rtx
{
    /// One microtriangle of a triangle a micromap has subdivided.
    struct Microtriangle
    {
        /// The three corners, in the barycentrics a candidate intersection reports: `x` weighs the
        /// triangle's second corner and `y` its third, and the first corner takes what is left.
        osg::Vec2f mCorners[3];

        /// Where the hardware reads this microtriangle's state.
        ///
        /// **Not the order they are walked in.** The states are laid out along a space-filling curve
        /// — the *bird curve*, so called for what it looks like — which is spatially coherent and
        /// bears no resemblance to the lattice a subdivision is naturally enumerated by.
        std::uint32_t mIndex = 0;
    };

    /// Where a point on a triangle lands in a micromap of `level`.
    ///
    /// **Transcribed from the specification's reference code and deliberately not re-derived.** The
    /// curve is defined recursively — into the sub-triangle nearest the first corner, then the
    /// middle one with its ordering flipped, then the second corner's, then the third's flipped —
    /// and the branchless bit arithmetic that implements it is the contract the hardware reads by.
    /// A version of it that was reasoned out afresh and happened to differ would put every state in
    /// the wrong place, and the geometry would still look plausible.
    std::uint32_t microtriangleIndexAt(float u, float v, std::uint32_t level);

    /// Cuts a triangle into the `4^level` microtriangles a micromap of that level describes.
    ///
    /// Written into `into` rather than returned, and the same for every triangle at a given level —
    /// so a mesh subdivides once and reads the answer for each of its thousands of triangles.
    void subdivideTriangle(std::uint32_t level, std::vector<Microtriangle>& into);
}
