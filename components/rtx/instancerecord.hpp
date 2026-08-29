#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Matrixf>

#include "scenedesc.hpp"

namespace Rtx
{
    /// An affine transform as three rows of four, translation in the last column.
    ///
    /// The shape an instance descriptor wants and the one OpenSceneGraph does not have.  OSG
    /// multiplies a row vector on the left, so its translation is the last *row*; a descriptor
    /// multiplies a column vector on the right, so the rotation is transposed and the translation
    /// moves to the last column. Getting that wrong mirrors the world about its diagonal, which is
    /// subtle enough on symmetrical architecture to survive being looked at — so the conversion
    /// happens once, here, and a backend only restates these rows in whatever order it stores them.
    struct Transform3x4
    {
        float mRows[3][4];

        bool operator==(const Transform3x4& other) const = default;
    };

    Transform3x4 toTransform3x4(const osg::Matrixf& matrix);

    /// One row of the top-level acceleration structure, with every decision already taken.
    ///
    /// **This is where the material policy lives, and it lives here once.** Which rays may see a
    /// surface, and whether traversal has to stop and ask whether a hit is a hole, are answers about
    /// Morrowind's content rather than about an API — and a backend working them out for itself
    /// would be a second place for them to be got wrong.
    struct InstanceRecord
    {
        Transform3x4 mTransform;

        /// World space to where this instance's world space was on the previous frame.
        ///
        /// **A single matrix rather than the previous transform**, so the shader multiplies once
        /// instead of inverting: `inverse(current) * previous`.
        ///
        /// **Set to the identity outright where the instance did not move**, rather than computed
        /// as an inverse times itself, which lands a few ulps away. `motion * p - p` is then
        /// bit-exactly zero and a static world produces no motion at all — see the cost of the
        /// alternative where it is built.
        Transform3x4 mMotion;

        /// The mesh whose bottom-level structure this places.
        Index mMesh = sNoIndex;

        /// Which rays are interested: `Shaders::MASK_SOLID`; `MASK_WATER` for a surface a shadow
        /// ray must pass straight through; or `MASK_FIRST_PERSON` for the player's own arms, which
        /// only the eye may meet. Sunlight reaching a seabed has come through the surface,

        /// so a sea that occluded would black out every shallow in the game — and saying it in the
        /// mask costs traversal nothing, where building the water non-opaque so a candidate loop
        /// could wave shadow rays past was measured at half the frame rate.
        std::uint32_t mMask = 0;

        /// Whether traversal must stop and ask the shader whether a hit is a hole.
        ///
        /// Without it the geometry's own opaque flag stands, traversal commits the first triangle it
        /// meets, and a canopy stays the rectangle it was painted on.
        bool mCutout = false;

        /// Whether traversal must stop and ask the shader how much of a hit there is.
        ///
        /// **Separate from `mCutout` because a micromap answers one and not the other.** A micromap
        /// resolves a microtriangle as opaque from the same mask a cutout is tested against, and a
        /// committed hit is the end of the ray — which is right for a leaf and loses a pane of glass
        /// entirely. So a translucent instance is forced non-opaque and is given no micromap.
        ///
        /// **Two ways to earn it.** The material says a pane of glass is one wherever it is placed.
        /// The placement says an actor the game is fading is one for as long as it fades, whatever
        /// its material claims — and that actor keeps its cutout, since a fade is not a hole.
        bool mTranslucent = false;

        /// Whether the slot this record sits in holds a placement.
        ///
        /// **Records are addressed by slot and slots have gaps**, because a slot index is what a hit
        /// reads back and closing a gap would rename every placement after it. A record that is not
        /// placed describes nothing and must not reach an acceleration structure.
        bool mPlaced = false;

        bool operator==(const InstanceRecord& other) const = default;
    };

    /// Fills `records` with one row per slot the scene holds, in slot order.
    ///
    /// Two invariants a backend inherits and must not restate differently. A record's position is
    /// the slot, which is the custom index the shader reads back at a hit — so a record with
    /// `mPlaced` false is a gap to be skipped and never renumbered away. And **every instance is
    /// built with face culling disabled**: Morrowind leans heavily on sheet geometry lit and hit
    /// from both faces, and a ray tracer has to be told, because back-face culling is not free for
    /// it the way a rasterizer's is.
    ///
    /// An out-parameter refilled in place, because a cell is thousands of instances and a rebuild
    /// must not go back to the allocator for a buffer it already had.
    void makeInstanceRecords(const SceneDesc& scene, std::vector<InstanceRecord>& records);

    /// Rewrites the rows of the slots the scene says changed — `getMoved` and `getSettled` — and
    /// leaves every other row as the last call left it.
    ///
    /// **What a frame costs, and it is what moved.** A record carries a matrix inverse and a
    /// nine-by-nine exterior is fifty thousand of them; building all of them again to change a
    /// hundred was most of what placing the world cost the CPU. `records` must be what
    /// `makeInstanceRecords` filled for this scene, and is grown here where the scene grew — a slot
    /// that arrived is in `getMoved`.
    /// Brings `records` up to what the scene now says, and names in `changed` every slot it wrote.
    ///
    /// **The one place the scene's change lists are read, and so the one place their order can be
    /// got wrong.** Every table a frame writes is derived from these records, and each used to
    /// subscribe to `getMoved` and `getSettled` for itself — which is two subscriptions to keep in
    /// step, in two files, with nothing saying they had to agree. They did not, and terrain stood a
    /// frame behind for it. What comes back in `changed` is what a backend writes; whether its own
    /// copies are then behind is `SlotTable`'s to know.
    ///
    /// `changed` is cleared and refilled, so a caller keeps one across frames and allocates none.
    /// A slot named twice is a row written twice, which costs a memcpy of one row.
    void updateInstanceRecords(
        const SceneDesc& scene, std::vector<InstanceRecord>& records, std::vector<Index>& changed);
}
