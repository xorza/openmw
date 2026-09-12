#pragma once

#include <cstdint>
#include <span>

#include <osg/BoundingBox>
#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include "runs.hpp"
#include "shaders/scene.h"
#include "shapefold.hpp"

namespace Rtx
{
    /// One mesh's vertices and its triangles, as everything that hands a mesh over states them.
    /// The attribute arrays are parallel: each is either empty or exactly as long as `mPositions`,
    /// and one vertex id indexes all of them. `mIndices` is mesh-local and a whole number of
    /// triangles.
    struct MeshArrays
    {
        std::span<const osg::Vec3f> mPositions;

        /// Empty where the geometry names no normal, which is read as "use the triangle's plane".
        std::span<const osg::Vec3f> mNormals;

        std::span<const osg::Vec2f> mTexCoords;

        /// The per-vertex colour, in linear light. Empty is white. Decoded where it is read and not
        /// where it is used, because a blend of display-encoded bytes is not the encoding of the
        /// blend and a hit interpolates across a triangle.
        std::span<const osg::Vec3f> mColours;

        std::span<const std::uint32_t> mIndices;
    };

    /// What a camera's cull mask sorts a placement by: the rasterizer's `Mask_Actor | Mask_Player`,
    /// `Mask_Effect` and `Mask_FirstPerson`, and `Static` for everything that states none of them.
    /// The innermost stated class stands for the path; the rasterizer culls on every stated mask
    /// along it, but no camera in the engine has a mask where the two differ.
    enum class InstanceClass : std::uint8_t
    {
        Static,
        Actor,
        Effect,
        FirstPerson,
    };

    /// The instance-mask bit a class is placed with. Water stands in for the class where the
    /// material is water, in `recordOf`.
    inline std::uint32_t classBit(const InstanceClass what)
    {
        switch (what)
        {
            case InstanceClass::Actor:
                return Shaders::MASK_ACTOR;
            case InstanceClass::Effect:
                return Shaders::MASK_EFFECT;
            case InstanceClass::FirstPerson:
                return Shaders::MASK_FIRST_PERSON;
            case InstanceClass::Static:
                break;
        }

        return Shaders::MASK_STATIC;
    }

    /// One mesh placed in the world: a row of the top-level acceleration structure.
    ///
    /// Not `Instance`, which in this namespace is the `VkInstance` a device comes from.
    struct MeshInstance
    {
        /// Object space to world space.
        osg::Matrixf mTransform;

        Index mMesh = sNoIndex;
        Index mMaterial = sNoIndex;

        /// How much of this placement is there — the fade the game is applying to one actor, with
        /// Invisibility and Chameleon, which ride the same pair of uniforms. On the placement and
        /// never on the material, because `SceneUtil::CopyOp` shares state sets and every actor
        /// built from one body part reads one material. One for nearly everything.
        float mOpacity = 1.0f;

        /// Which class of thing this is, for a camera's cull mask to keep or leave out. The
        /// innermost node on its path that stated a class; `Static` where none did.
        InstanceClass mClass = InstanceClass::Static;

        /// Whether this slot holds anything. A dropped placement leaves its slot behind rather than
        /// closing the gap, because the slot index is what a hit reads back.
        bool isPlaced() const { return mMesh != sNoIndex; }
    };

    /// How a mesh's vertices are re-posed every frame, where they are: by bone rows through a rig,
    /// by target weights through a morph, or not at all. `NifOsg` never makes a geometry both.
    enum class Deform : std::uint8_t
    {
        None,
        Rig,
        Morph,
    };

    /// Where one mesh's vertices and indices sit in the scene's shared buffers — shared because a
    /// cell holds thousands of meshes and the GPU wants one buffer anyway.
    struct MeshRange
    {
        /// Where this mesh's vertices sit, which the positions, the normals and the texture
        /// coordinates are all indexed by. The run the allocator handed out, given back as it stands.
        Run mVertices;

        /// Where its indices sit. They are mesh-local, so a triangle's vertex is
        /// `mVertices.mOffset` plus what the index says.
        Run mIndices;

        /// What the fold found this mesh's triangles to be. `Rtx::FoldedShape` says what each half
        /// means; the scene keeps them and draws nothing from them.
        FoldedShape mShape;

        /// Whether this mesh is re-posed by `poseRig` or `poseMorph` — a skinned body, a morphed
        /// face — which is what tells a backend to build its structure so it can be refitted rather
        /// than built again, and which kernel poses it. The caller's finding, like `mShape`.
        Deform mDeform = Deform::None;

        /// The rig or the morph that poses it, into `getRigs` or `getMorphs`. `sNoIndex` for a mesh
        /// that stands.
        Index mDeformer = sNoIndex;

        /// The material this mesh arrived wearing, or `sNoIndex` for one that arrived with none. A
        /// mesh's and not a placement's, because `SceneUtil::CopyOp` shares drawables and state
        /// sets, so a hundred crates are one drawable and one state set. A placement wearing
        /// another is `ExtractionStats::mWornOtherwise`, and the loader says there is none.
        Index mMaterial = sNoIndex;

        /// Where this mesh's bind pose sits among the deforming meshes' vertices, which is what a
        /// backend's bind table is indexed by. **A run as long as `mVertices` beside the mesh's
        /// own**, allocated only for a mesh that deforms: the shared vertex buffers hold every mesh,
        /// and a bind table that mirrored them would hold megabytes of the cell for a few bodies.
        Index mBindOffset = 0;

        /// Where this mesh's bone rows or morph weights start in `getBones` or `getWeights`. The
        /// count is the rig's or the morph's.
        Index mPoseOffset = 0;

        /// Whether a pose has been written since the mesh arrived. The first pose names the mesh
        /// whatever it is, so a body whose first pose happens to equal the zeroed rows still
        /// reaches the device.
        bool mPosed = false;

        /// The box this mesh's vertices fit in, in the space they are stated in. Invalid where the
        /// slot is free. Taken once as the vertices arrive, and for a mesh that deforms from what
        /// the caller says the pose reaches, because the posed vertices are on the device; that is
        /// what lets a question about where a scene reaches be eight transforms per instance.
        osg::BoundingBoxf mBounds;

        Index getTriangleCount() const { return mIndices.mCount / 3; }
    };

    /// How many instances a scene places, and how many of those each kind of traversal has to stop
    /// for — kept by the row that changed rather than recounted over the table, and handed on
    /// unchanged to whoever reports them.
    struct InstanceCounts
    {
        std::uint32_t mPlaced = 0;

        /// How many of those traversal has to stop and ask where the holes are, so a material
        /// change that marks half a cell non-opaque shows up before a frame time does. A translucent
        /// instance is stopped for as well and counted here nowhere, since it never ends the ray.
        std::uint32_t mCutout = 0;

        /// How many of them the eye meets as water — what says whether a trace needs the sea at
        /// all, and what `HAS_SEA` removes from a room's kernel.
        std::uint32_t mWater = 0;

        /// How many of them are a medium the eye passes through — `Rtx::Material::isMedium` — and
        /// so whether `mediumAlong` has to walk the structure at all
        /// (`VisibilityConstants::mMediumInFrame`).
        std::uint32_t mMedium = 0;
    };
}
