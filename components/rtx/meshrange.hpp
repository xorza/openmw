#pragma once

#include <cstdint>

#include <osg/BoundingBox>

#include "index.hpp"
#include "run.hpp"
#include "shapefold.hpp"

namespace Rtx
{
    /// How a mesh's vertices are re-posed every frame, where they are.
    ///
    /// **The kind names the kernel and the pose it takes.** A skinned body is posed by bone rows
    /// through a rig, a morphed face by target weights through a morph, and a mesh that stands is
    /// neither. The loader never makes a geometry both: `NifOsg` skips the morpher where a skin
    /// exists.
    enum class Deform : std::uint8_t
    {
        None,
        Rig,
        Morph,
    };

    /// Where one mesh's vertices and indices sit in the scene's shared buffers.
    ///
    /// The buffers are shared rather than per-mesh because a cell holds thousands of meshes and a
    /// `vector` of `vector`s would pay an allocation for each one — and because the GPU wants one
    /// buffer anyway, so a per-mesh vector would only have to be flattened again on the way up.
    struct MeshRange
    {
        /// Where this mesh's vertices sit, which the positions, the normals and the texture
        /// coordinates are all indexed by.
        ///
        /// **The run the allocator handed out, kept as it was handed out.** It is given back exactly
        /// as it stands, so taking it apart into an offset and a count only means building it again
        /// to release it.
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

        /// The material this mesh arrived wearing, or `sNoIndex` for one that arrived with none.
        ///
        /// **A mesh's, and not a placement's, because a static mesh wears one material by
        /// construction.** `SceneUtil::CopyOp` copies nodes and shares drawables and state sets, so
        /// a hundred crates are a hundred nodes over one drawable and one state set — the material
        /// the extractor keys on is the same object under every placement. What a backend bakes
        /// against the mask a mesh is worn with, it bakes against this; a placement wearing
        /// another is `ExtractionStats::mWornOtherwise`, and the loader says there is none. The
        /// caller's finding, like `mShape`.
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
        /// slot is free.
        ///
        /// **Written where the vertices are, and nowhere else.** A mesh's own extent is a fact about
        /// its positions, so it is taken once as they arrive — and for a mesh that deforms, taken
        /// again from what the caller says the pose reaches, because the posed vertices are on the
        /// device and nowhere else. That is what lets a question about where a scene reaches be
        /// eight transforms per instance rather than a walk over every vertex in the table.
        osg::BoundingBoxf mBounds;

        Index getTriangleCount() const { return mIndices.mCount / 3; }
    };
}
