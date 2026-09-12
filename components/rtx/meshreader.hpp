#pragma once

#include <span>
#include <vector>

#include <osg/Vec3f>

#include "geometryfold.hpp"
#include "mesh.hpp"
#include "nodekind.hpp"
#include "shapefold.hpp"

namespace osg
{
    class Drawable;
    class Geometry;
}

namespace SceneUtil
{
    class MorphGeometry;
    class RigGeometry;
}

namespace Rtx
{
    /// What of a drawable there is to mirror: the geometry its triangles and attributes are read
    /// from, and what poses it. A skinned body's geometry is its source — the bind pose, which a
    /// pose is computed from on the device — and so is a morphed face's, with the base target for
    /// its positions. Neither is the double-buffered copy a cull writes, which nothing runs here.
    struct DrawableRead
    {
        const osg::Geometry* mGeometry = nullptr;
        Deform mDeform = Deform::None;
        const SceneUtil::RigGeometry* mRig = nullptr;
        const SceneUtil::MorphGeometry* mMorph = nullptr;
    };

    /// Reads what a drawable is, in one virtual call for nearly everything in a cell. A skinned
    /// body and a morphed face are an `osg::Drawable` over a source geometry, and the source is
    /// what this reads. A rig no update traversal resolved is read as it stands, because its bones
    /// are what `RigGeometry::updateBounds` finds, and the rasterizer draws it in its bind pose
    /// too. A morph with no target past its base is a static mesh whose positions are the base.
    DrawableRead readDrawable(const osg::Drawable& drawable, NodeKind kind);

    /// A morph's base target, which `MorphGeometry::cull` reads its positions from.
    std::span<const osg::Vec3f> morphBase(const SceneUtil::MorphGeometry& morph);

    /// What one drawable's triangles come to once read and folded: spans into the geometry's own
    /// arrays and into the reader's scratch, valid until the reader reads again.
    struct MeshReading
    {
        /// The vertices, and the triangles the fold kept. An attribute the geometry carries none
        /// of — or one of another length — comes back empty.
        MeshArrays mArrays;

        FoldedShape mShape;

        /// What the fold cost, in milliseconds. `ExtractionStats::mFoldMs` is where it is summed.
        double mFoldMs = 0.0;
    };

    /// Turns a drawable into a `MeshReading`: the half of a mesh's arrival that reads and folds,
    /// apart from the half that inserts, because the ring does the first on a thread of its own
    /// and what a shape is has to be one answer. Not thread-safe, and one instance a thread.
    class MeshReader
    {
    public:
        /// Reads `read` into `into`. False where the drawable holds no triangle to read. Throws
        /// where a morph's base is not the length of its source, which is a content file naming a
        /// face this cannot pose.
        bool read(const DrawableRead& read, MeshReading& into);

    private:
        GeometryFold mFold;

        /// Where an overall normal is spread across a drawable's vertices.
        std::vector<osg::Vec3f> mFlatNormalScratch;

        /// Where a drawable's colours are decoded to. Always scratch, where the other
        /// attributes are usually the geometry's own arrays: what the file holds is display-encoded
        /// bytes and what a hit interpolates is linear light, so there is nothing to point at.
        std::vector<osg::Vec3f> mColourScratch;
    };
}
