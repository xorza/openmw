#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include "geometryfold.hpp"
#include "meshrange.hpp"
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
    /// from, and what poses it, where something does.
    ///
    /// A skinned body's geometry is its **source** — the bind pose, which is what a pose is
    /// computed from on the device — and a morphed face's is its source too, with the base target
    /// standing in for its positions. Neither is the double-buffered copy a cull writes, which
    /// nothing here runs any more.
    struct DrawableRead
    {
        const osg::Geometry* mGeometry = nullptr;
        Deform mDeform = Deform::None;
        const SceneUtil::RigGeometry* mRig = nullptr;
        const SceneUtil::MorphGeometry* mMorph = nullptr;
    };

    /// Reads what a drawable is, in one virtual call for nearly everything in a cell.
    ///
    /// Nearly everything in a cell is an `osg::Geometry` and answers in one virtual call. A skinned
    /// body and a morphed face are not: each is an `osg::Drawable` over a source geometry, and the
    /// source is what this reads. **A rig no update traversal has resolved is read as it stands.**
    /// Its bones are what `RigGeometry::updateBounds` finds under the update traversal, and a rig
    /// with none has nothing to be posed against; the rasterizer draws that rig in its bind pose,
    /// and so does this. A morph with no target past its base has nothing to move either, and is a
    /// static mesh whose positions are the base.
    DrawableRead readDrawable(const osg::Drawable& drawable);

    /// A morph's base target, which `MorphGeometry::cull` reads its positions from.
    std::span<const osg::Vec3f> morphBase(const SceneUtil::MorphGeometry& morph);

    /// What one drawable's triangles come to once read and folded.
    ///
    /// **Spans and not copies**, into the geometry's own arrays and into the reader's scratch:
    /// valid until the reader reads again, and for as long as the geometry stands. A caller that
    /// keeps a reading past either copies it.
    struct MeshReading
    {
        std::span<const osg::Vec3f> mPositions;

        /// Empty where the geometry names no normal.
        std::span<const osg::Vec3f> mNormals;

        /// Empty where the geometry carries none, or one of another length.
        std::span<const osg::Vec2f> mTexCoords;

        /// The triangles the fold kept.
        std::span<const std::uint32_t> mIndices;

        FoldedShape mShape;

        /// What the fold cost, in milliseconds. `ExtractionStats::mFoldMs` is where it is summed.
        double mFoldMs = 0.0;
    };

    /// Turns a drawable into a `MeshReading`.
    ///
    /// **The half of a mesh's arrival that reads and folds, apart from the half that inserts.** The
    /// mirror does both on the frame for a drawable the walk has just met; a ring preparing cells
    /// ahead of the eye does the first on a thread of its own and hands the frame the second — and
    /// what a shape is has to be one answer wherever it is asked, so this is the one place it is.
    ///
    /// **Not thread-safe, and one instance a thread.** Everything it keeps is scratch it overwrites.
    class MeshReader
    {
    public:
        /// Reads `read` into `into`.
        ///
        /// Throws where a morph's base is not the length of its source, which is a content file
        /// naming a face this cannot pose.
        ///
        /// @return false where the drawable holds no triangle to read, which is a drawable that
        ///         mirrors nothing.
        bool read(const DrawableRead& read, MeshReading& into);

    private:
        GeometryFold mFold;

        /// Where an overall normal is spread across a drawable's vertices.
        std::vector<osg::Vec3f> mFlatNormalScratch;
    };
}
