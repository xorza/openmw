// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SKINNING_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SKINNING_H

// What poses a skinned body or a morphed face on the device, as both sides see it. Included
// verbatim by the host and by the two kernels, for the reason `scene.h` is: a row the host packs and
// a kernel reads has to be one row.
//
// **The arithmetic is `SceneUtil::RigGeometry::cull`'s and `MorphGeometry::cull`'s, and nothing
// else.** A skin is `p · Σ w_i B_i` with `B_i` the bone's inverse bind, its skeleton-space matrix and
// the skin transform composed on the host; a normal takes the linear part of the same sum and is not
// normalised, because the rasterizer does not. A morph is the base plus every target's offset at its
// weight, positions only. What the game draws is the target, and these are its numbers.

#ifdef __cplusplus

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

    /// Lanes in one workgroup of either kernel. A body is a few thousand vertices, so a dispatch is
    /// tens of groups.
    const uint SKIN_WORKGROUP = 64;

    /// A vertex's run word: the run's first influence in the low bits above the count, and the
    /// count in the bottom byte. `RigGeometry::VertexList` is `unsigned short`, so a rig's runs are
    /// under sixty-four thousand and its influences a small multiple of that, which leaves the
    /// twenty-four bits of `first` three orders of magnitude of room.
    const uint RUN_COUNT_BITS = 8u;
    const uint RUN_COUNT_MASK = 0xFFu;

    /// One bone's share of one vertex.
    ///
    /// **A run per vertex and not a fixed four**, because the rasterizer applies every influence
    /// the file names. The reference implementation measured 107 of 487 000 vertices with a fifth
    /// and dropped it; here the run costs an indirection and makes the answer exact.
    struct GpuInfluence
    {
        /// Into the rig's bones, and so into the mesh's rows of `GpuBone`.
        uint mBone;
        float mWeight;
    };

    /// One bone's pose for one mesh: mesh space to mesh space, as three rows of four.
    ///
    /// Row `i` is `(M(0,i), M(1,i), M(2,i), M(3,i))` of the OpenSceneGraph matrix, which is how
    /// `Rtx::toTransform3x4` packs a transform — so `dot(mRows[i], vec4(p, 1))` is `p · M` and the
    /// same packing serves an instance's motion and a bone.
    struct GpuBone
    {
        vec4 mRows[3];

#ifdef __cplusplus
        /// For telling a pose from the one already held, row by row.
        bool operator==(const GpuBone& other) const = default;
#endif
    };

    /// What one skinned mesh's dispatch is handed: where its bind pose, its rig and its rows are,
    /// and where the pose goes.
    ///
    /// **Addresses of the runs themselves and not of the tables**, so the kernel indexes from
    /// nought. `Rtx::SceneDesc` never lets a run straddle a block, so one address covers it.
    struct SkinConstants
    {
        uint64 mBindPositions;
        uint64 mBindNormals;
        uint64 mRuns;
        uint64 mInfluences;
        uint64 mBones;
        uint64 mPositions;
        uint64 mNormals;
        uint mCount;

        /// Explicit, so the range a pipeline declares and the struct a host writes are one size.
        uint mPadding;
    };

    /// The same for a morphed mesh: its base, every target's offsets laid end to end, this frame's
    /// weights, and where the positions go. A morph moves no normal.
    struct MorphConstants
    {
        uint64 mBase;
        uint64 mOffsets;
        uint64 mWeights;
        uint64 mPositions;
        uint mCount;
        uint mTargets;
    };

#ifdef __cplusplus

    static_assert(sizeof(GpuInfluence) == 8, "GpuInfluence must be scalar-packed on every side");
    static_assert(sizeof(GpuBone) == 48, "GpuBone must be scalar-packed on every side");
    static_assert(sizeof(SkinConstants) == 64, "SkinConstants must be scalar-packed on every side");
    static_assert(sizeof(MorphConstants) == 40, "MorphConstants must be scalar-packed on every side");
}

#else

#undef uint64

#endif

#endif
