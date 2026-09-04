// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_MICROMAP_H
#define OPENMW_COMPONENTS_RTX_SHADERS_MICROMAP_H

#include "portable.h"

// What an opacity micromap promises, as both sides see it. Included verbatim by the host, which
// chooses every triangle's level and writes the build's usage counts, and by the kernel that decides
// every microtriangle's state — for the reason `skinning.h` is: the curve that orders the
// microtriangles and the rule that decides one have to be one curve and one rule.
//
// **The curve is the specification's own.** `VK_EXT_opacity_micromap` states it as reference code
// from barycentrics to an index; `microtriangleIndex` is that arithmetic on the integer cell, and
// `microtriangleAt` is its inverse as a descent — the four children of a triangle in the order the
// curve visits them, with the two it visits mirrored. `RtxMicromapCurveTest` is what says the two
// agree at every level the kernel bakes.

#ifdef RTX_HOST

#include <cstdint>

#include <osg/Vec4f>

namespace Rtx::Shaders
{
    using vec4 = osg::Vec4f;
    using uint = std::uint32_t;
    using uint64 = std::uint64_t;

#else

#define uint64 uint64_t

#endif

    /// Lanes in one workgroup of the bake: one lane per triangle, so a mesh is tens of groups.
    const uint MICROMAP_WORKGROUP = 64;

    /// The subdivision levels a triangle may be baked at.
    ///
    /// **Two and not one at the bottom, because the kernel stores words.** A level-one triangle is
    /// four states in one byte, and a byte at an arbitrary offset is not a store a lane can make
    /// without eight-bit storage; padded to a word it costs what a level-two triangle costs and
    /// answers for a quarter as many microtriangles. So the floor is the level whose data is one
    /// word, and every triangle's run is a whole number of them.
    ///
    /// **Six at the top is the SDK's cap**, which the memory arithmetic makes necessary: four to
    /// the level in states per triangle, so each level costs four times the one under it, and at
    /// six a town's micromaps measured an eighth to a fifth of its structures' own size.
    const uint MICROMAP_LEVEL_MIN = 2;
    const uint MICROMAP_LEVEL_MAX = 6;

    /// The four states of the four-state format, as the specification numbers them: a transparent
    /// microtriangle is ignored without an any-hit, an opaque one commits, and either unknown runs
    /// the any-hit as non-opaque — unless a ray or an instance asks for two states, which folds each
    /// unknown to its half.
    const uint MICROMAP_TRANSPARENT = 0;
    const uint MICROMAP_OPAQUE = 1;
    const uint MICROMAP_UNKNOWN_TRANSPARENT = 2;
    const uint MICROMAP_UNKNOWN_OPAQUE = 3;

    /// Bits one state takes in the four-state format, packed least significant first, and so how
    /// many states one word holds.
    const uint MICROMAP_STATE_BITS = 2;
    const uint MICROMAP_STATES_PER_WORD = 16;

    /// The most texels one microtriangle's support may read before the bake leaves it unknown.
    ///
    /// **What bounds a lane's work.** A triangle's level is chosen so that a microtriangle is a
    /// few texels across, and the cap on the level leaves whatever covers more than the cap can cut
    /// over as many texels as it likes: a rope with its texture repeated a hundred times along it
    /// is one triangle over a million texels, and a lane reading every one of them for each of its
    /// four thousand microtriangles is the device's watchdog firing on a cell crossing — measured.
    /// Past this a microtriangle is unknown, which the any-hit decides exactly as it does today,
    /// and only the half it folds to is read, on a sparse grid.
    const uint MICROMAP_TEXEL_BUDGET = 64;

    /// Samples along each axis of a footprint past the budget, for the mean that folds it. Placed
    /// at the centres of that many bins across each span, in integer arithmetic both sides share.
    const uint MICROMAP_SPARSE_SAMPLES = 4;

    /// How many microtriangles a triangle at `level` is cut into.
    RTX_SHADER uint microtriangleCount(uint level)
    {
        return 1u << (2u * level);
    }

    /// How many words a triangle's four-state data takes at `level`, which is at least
    /// `MICROMAP_LEVEL_MIN` — the level at which that is one word.
    RTX_SHADER uint microtriangleWords(uint level)
    {
        return microtriangleCount(level) / MICROMAP_STATES_PER_WORD;
    }

    /// Where a microtriangle stands along the curve, from the cell it is in.
    ///
    /// The triangle is cut into a grid of `2^level` steps along `u` and `v`; a cell `(iu, iv)` with
    /// `iu + iv < 2^level` holds an upright microtriangle, with the cell's corner as its own, and a
    /// cell with `iu + iv < 2^level - 1` holds a flipped one as well, filling the rest of the
    /// square. The arithmetic is the specification's reference code with the quantisation taken
    /// off the front, which is the only part of it a lane walking cells has no use for.
    RTX_SHADER uint microtriangleIndex(uint level, uint iu, uint iv, bool flipped)
    {
        const uint mask = (1u << level) - 1u;

        // The third coordinate, as the reference code spells it: the complement of the sum, and
        // one less for a flipped microtriangle, whose three coordinates sum to one less.
        uint iw = ~(iu + iv);
        if (flipped)
            iw -= 1u;

        uint b0 = ~(iu ^ iw) & mask;
        const uint t = (iu ^ iv) & b0;

        uint f = t;
        f ^= f >> 1u;
        f ^= f >> 2u;
        f ^= f >> 4u;
        f ^= f >> 8u;
        uint b1 = ((f ^ iu) & ~b0) | t;

        b0 = (b0 | (b0 << 8u)) & 0x00ff00ffu;
        b0 = (b0 | (b0 << 4u)) & 0x0f0f0f0fu;
        b0 = (b0 | (b0 << 2u)) & 0x33333333u;
        b0 = (b0 | (b0 << 1u)) & 0x55555555u;
        b1 = (b1 | (b1 << 8u)) & 0x00ff00ffu;
        b1 = (b1 | (b1 << 4u)) & 0x0f0f0f0fu;
        b1 = (b1 | (b1 << 2u)) & 0x33333333u;
        b1 = (b1 | (b1 << 1u)) & 0x55555555u;

        return b0 | (b1 << 1u);
    }

    /// One microtriangle's three corners, in the grid of `2^level` steps along `u` and `v` that
    /// `microtriangleIndex` counts cells in.
    ///
    /// **Ordered, and the order is the curve's.** Which corner is first is what says how the
    /// microtriangle's own children are visited, so a corner is never the same as another corner
    /// with the names swapped — which is also why this is six numbers and not a cell and a flag.
    struct Microtriangle
    {
        uint mU0;
        uint mV0;
        uint mU1;
        uint mV1;
        uint mU2;
        uint mV2;
    };

    /// The microtriangle at `index` along the curve, as its corners.
    ///
    /// The descent the reference code's bit arithmetic amounts to: each pair of bits from the top
    /// picks one of a triangle's four children, and the child's own corners are named so that the
    /// next pair is read the same way. From a triangle `(V0, V1, V2)` with `Mij` the midpoint of
    /// `Vi` and `Vj`, the children in curve order are the corner at `V0` as `(V0, M01, M20)`, the
    /// middle as `(M20, M12, M01)`, the corner at `V1` as `(M01, V1, M12)`, and the corner at `V2`
    /// as `(M12, M20, V2)` — the middle and the last mirrored, which is what makes the curve
    /// continuous.
    RTX_SHADER Microtriangle microtriangleAt(uint level, uint index)
    {
        const uint whole = 1u << level;

        uint u0 = 0u;
        uint v0 = 0u;
        uint u1 = whole;
        uint v1 = 0u;
        uint u2 = 0u;
        uint v2 = whole;

        for (uint step = level; step > 0u; --step)
        {
            const uint child = (index >> (2u * (step - 1u))) & 3u;

            const uint mu01 = (u0 + u1) >> 1u;
            const uint mv01 = (v0 + v1) >> 1u;
            const uint mu12 = (u1 + u2) >> 1u;
            const uint mv12 = (v1 + v2) >> 1u;
            const uint mu20 = (u2 + u0) >> 1u;
            const uint mv20 = (v2 + v0) >> 1u;

            if (child == 0u)
            {
                u1 = mu01;
                v1 = mv01;
                u2 = mu20;
                v2 = mv20;
            }
            else if (child == 1u)
            {
                u0 = mu20;
                v0 = mv20;
                u1 = mu12;
                v1 = mv12;
                u2 = mu01;
                v2 = mv01;
            }
            else if (child == 2u)
            {
                u0 = mu01;
                v0 = mv01;
                u2 = mu12;
                v2 = mv12;
            }
            else
            {
                u0 = mu12;
                v0 = mv12;
                u1 = mu20;
                v1 = mv20;
            }
        }

        Microtriangle corners;
        corners.mU0 = u0;
        corners.mV0 = v0;
        corners.mU1 = u1;
        corners.mV1 = v1;
        corners.mU2 = u2;
        corners.mV2 = v2;

        return corners;
    }

    /// What a microtriangle promises, from the alpha of every texel whose bilinear support touches
    /// its footprint at level zero.
    ///
    /// **Conservative against `sampleDiffuse` at the finest level, and exact there.** A bilinear
    /// blend never leaves the range of the texels it blends, so where the least of them is at or
    /// above the cutoff every sample in the footprint is, and where the greatest is below it none
    /// is; every shadow ray answers as it did through the any-hit. Between the two the any-hit has
    /// to decide, and the mean says which half a two-state ray folds the unknown to — opaque where
    /// the footprint is mostly there, as the SDK recommends.
    ///
    /// @param sum,count the texels' alpha summed and how many there were, compared as a product so
    ///        that the two sides need no division to agree about.
    RTX_SHADER uint microtriangleState(float least, float most, float sum, float count, float cutoff)
    {
        if (least >= cutoff)
            return MICROMAP_OPAQUE;
        if (most < cutoff)
            return MICROMAP_TRANSPARENT;

        return sum >= cutoff * count ? MICROMAP_UNKNOWN_OPAQUE : MICROMAP_UNKNOWN_TRANSPARENT;
    }

    /// What one mesh's bake is handed: where its triangles and their texture coordinates are, the
    /// triangle array the host wrote for the build, where the data goes, and the material the mesh
    /// is worn with.
    ///
    /// **Addresses of the runs themselves and not of the tables**, so the kernel indexes from
    /// nought. `Rtx::SceneDesc` never lets a run straddle a block, so one address covers it.
    struct MicromapConstants
    {
        uint64 mIndices;
        uint64 mTexCoords;

        /// One `VkMicromapTriangleEXT` per triangle, as the host wrote it for the build: the data
        /// offset in bytes, then the level and the format packed in one word. The kernel reads its
        /// level and its offset from here so that the two cannot be stated twice.
        uint64 mTriangles;
        uint64 mData;

        /// Mesh texture coordinates to the material's, as `uv * xy + zw`.
        vec4 mTransform;

        /// The material's diffuse slot in the bindless array, whose alpha is the mask.
        uint mTexture;

        uint mCount;
        float mCutoff;

        /// Explicit, so the range a pipeline declares and the struct a host writes are one size.
        uint mPadding;
    };

#ifdef RTX_HOST

    static_assert(sizeof(MicromapConstants) == 64, "MicromapConstants must be scalar-packed on every side");
}

#else

#undef uint64

#endif

#endif
