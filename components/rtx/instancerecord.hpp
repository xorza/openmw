#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Matrixf>

#include "scenedesc.hpp"

namespace Rtx
{
    /// An affine transform as three rows of four, translation in the last column — the shape an
    /// instance descriptor wants, where OSG's translation is the last *row*. Getting that wrong
    /// mirrors the world about its diagonal, so the conversion happens once, here.
    struct Transform3x4
    {
        float mRows[3][4];

        bool operator==(const Transform3x4& other) const = default;
    };

    Transform3x4 toTransform3x4(const osg::Matrixf& matrix);

    /// A bone's pose as the skinning kernel reads it: the same three rows, packed once here so a bone
    /// and an instance's motion cannot disagree about which way round a matrix goes.
    Shaders::GpuBone toGpuBone(const osg::Matrixf& matrix);

    /// One row of the top-level acceleration structure, with every decision already taken: which
    /// rays may see a surface and whether traversal has to stop are answers about Morrowind's
    /// content and not about an API.
    struct InstanceRecord
    {
        Transform3x4 mTransform;

        /// World space to where this instance's world space was on the previous frame:
        /// `inverse(current) * previous`, so the shader multiplies once. The identity outright
        /// where the instance did not move, so a static world produces motion that is bit-exactly
        /// zero.
        Transform3x4 mMotion;

        /// The mesh whose bottom-level structure this places.
        Index mMesh = sNoIndex;

        /// What shading a hit on this instance takes — the material's own kind, carried on the
        /// placement so that traversal picks the shader through the shader-table record offset
        /// instead of the shader reading a material row.
        MaterialKind mKind = MaterialKind::Surface;

        /// Which rays are interested: the class bit `InstanceClass` gives it, or `MASK_WATER` for a
        /// surface a shadow ray must pass straight through, or every shallow in the game goes
        /// black; `MASK_MEDIUM` beside either. Said in the mask, because a candidate loop waving
        /// shadow rays past costs half the frame rate.
        std::uint32_t mMask = 0;

        /// Whether traversal must stop and ask the shader whether a hit is a hole. Without it the
        /// geometry's own opaque flag stands, traversal commits the first triangle it meets, and a
        /// canopy stays the rectangle it was painted on.
        bool mCutout = false;

        /// Whether traversal must stop and ask the shader how much of a hit there is — separate
        /// from `mCutout`, which asks whether there is anything at the hit at all. Earned by the
        /// material, for a pane of glass, or by the placement, for an actor the game is fading.
        bool mTranslucent = false;

        /// Whether the slot this record sits in holds a placement. Records are addressed by slot
        /// and slots have gaps, because a slot index is what a hit reads back.
        bool mPlaced = false;

        bool operator==(const InstanceRecord& other) const = default;
    };

    /// Fills `records` with one row per slot the scene holds, in slot order — a record with
    /// `mPlaced` false is a gap to be skipped and never renumbered away. Every instance is built
    /// with face culling disabled, because Morrowind leans on sheet geometry lit and hit from both
    /// faces. An out-parameter refilled in place.
    void makeInstanceRecords(const SceneDesc& scene, std::vector<InstanceRecord>& records);

    /// Rewrites the rows of the slots the scene says changed — `getMoved` and `getSettled` — leaves
    /// every other row as the last call left it, and names in `changed` every slot it wrote. A
    /// nine-by-nine exterior is fifty thousand matrix inverses, and a frame changes a hundred. The
    /// one place the scene's change lists are read, so every table a frame writes derives from one
    /// answer. `records` must be what `makeInstanceRecords` filled, grown here where the scene
    /// grew; `changed` is cleared and refilled.
    void updateInstanceRecords(
        const SceneDesc& scene, std::vector<InstanceRecord>& records, std::vector<Index>& changed);
}
