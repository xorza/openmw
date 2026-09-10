#include "meshreader.hpp"

#include <cassert>
#include <chrono>
#include <string>

#include <osg/Array>
#include <osg/Drawable>
#include <osg/Geometry>

#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>

#include "error.hpp"
#include "frameclock.hpp"

namespace Rtx
{
    namespace
    {
        /// A geometry's per-vertex positions and normals.
        struct VertexArrays
        {
            std::span<const osg::Vec3f> mPositions;

            /// Empty only where the geometry names no normal at all. A per-vertex array is taken as
            /// it stands and a single overall one is spread across the vertices, which is the same
            /// answer at every point of a flat surface.
            std::span<const osg::Vec3f> mNormals;
        };

        /// The array as a `Vec3Array`, or null where it is anything else.
        ///
        /// **`osg::Array` states its own type in a byte**, which is what a `dynamic_cast` walks the
        /// class hierarchy to work out — the same shape as the library-name test the walk already
        /// makes of a drawable and of a terrain chunk before it casts either.
        const osg::Vec3Array* asVec3Array(const osg::Array* array)
        {
            if (array == nullptr || array->getType() != osg::Array::Vec3ArrayType)
                return nullptr;

            return static_cast<const osg::Vec3Array*>(array);
        }

        /// @param flat scratch for an overall normal spread across the vertices. Refilled here and
        ///        borrowed by the returned span, so it has to outlive the read.
        VertexArrays readVertices(const osg::Geometry& geometry, std::vector<osg::Vec3f>& flat)
        {
            VertexArrays arrays;

            const osg::Vec3Array* positions = asVec3Array(geometry.getVertexArray());
            if (positions == nullptr)
                return arrays;

            arrays.mPositions = std::span(positions->asVector());

            const osg::Vec3Array* normals = asVec3Array(geometry.getNormalArray());
            if (normals == nullptr || normals->empty())
                return arrays;

            if (normals->size() == positions->size())
            {
                arrays.mNormals = std::span(normals->asVector());
                return arrays;
            }

            // **One normal for the whole drawable is a normal, and dropping it made the sea flat
            // black.** `SceneUtil::createWaterGeometry` binds exactly this — a thousand vertices and
            // one `(0, 0, 1)` — so the game's water mirrored with no normal at all, and shading a
            // surface by a zero vector produces radiance that the frame's own exposure then reads.
            // Everything else in the picture goes with it.
            if (normals->getBinding() != osg::Array::BIND_OVERALL)
                return arrays;

            flat.assign(positions->size(), normals->at(0));
            arrays.mNormals = std::span(flat);
            return arrays;
        }

        /// The array as a `Vec2Array`, or null where it is anything else. `asVec3Array` says why
        /// the type byte and not a `dynamic_cast`.
        const osg::Vec2Array* asVec2Array(const osg::Array* array)
        {
            if (array == nullptr || array->getType() != osg::Array::Vec2ArrayType)
                return nullptr;

            return static_cast<const osg::Vec2Array*>(array);
        }
    }

    DrawableRead readDrawable(const osg::Drawable& drawable)
    {
        if (const osg::Geometry* geometry = drawable.asGeometry())
            return DrawableRead{ .mGeometry = geometry };

        if (const auto* rig = dynamic_cast<const SceneUtil::RigGeometry*>(&drawable))
        {
            const bool skinned = rig->getInfluenceData() != nullptr && !rig->getBones().empty();
            return DrawableRead{ .mGeometry = rig->getSourceGeometry().get(),
                .mDeform = skinned ? Deform::Rig : Deform::None,
                .mRig = rig };
        }

        if (const auto* morph = dynamic_cast<const SceneUtil::MorphGeometry*>(&drawable))
        {
            const bool moving = morph->getMorphTargetList().size() > 1;
            return DrawableRead{ .mGeometry = morph->getSourceGeometry().get(),
                .mDeform = moving ? Deform::Morph : Deform::None,
                .mMorph = morph };
        }

        return DrawableRead{};
    }

    std::span<const osg::Vec3f> morphBase(const SceneUtil::MorphGeometry& morph)
    {
        // The source geometry's own array is what `NifOsg` built the drawable from and the two agree
        // in every file it builds, so the length is asserted where it is read against the source.
        const osg::Vec3Array* base = morph.getMorphTarget(0).getOffsets();
        assert(base != nullptr && "a morph whose base is no array");
        return std::span(base->asVector());
    }

    bool MeshReader::read(const DrawableRead& read, MeshReading& into)
    {
        const osg::Geometry& geometry = *read.mGeometry;

        VertexArrays arrays = readVertices(geometry, mFlatNormalScratch);

        // A morph starts from its base target and not from the source's array, because that is
        // what `MorphGeometry::cull` starts from. The normals and everything else are the source's.
        if (read.mDeform == Deform::Morph)
        {
            const std::span<const osg::Vec3f> base = morphBase(*read.mMorph);
            if (base.size() != arrays.mPositions.size())
                throw Error("a morphed face of " + std::to_string(arrays.mPositions.size())
                    + " vertices whose base target has " + std::to_string(base.size()));

            arrays.mPositions = base;
        }

        if (arrays.mPositions.empty())
            return false;

        // Folded before the mesh is written, so the copy the content drew for a card's back never
        // reaches a structure. Once per drawable and never for a pose: a rig moves the two copies
        // together, so the pairs found in the bind pose are the pairs.
        const std::chrono::steady_clock::time_point folding = std::chrono::steady_clock::now();
        const bool folded = mFold.read(geometry, arrays.mPositions, into.mShape);
        into.mFoldMs = since(folding, std::chrono::steady_clock::now());

        if (!folded)
            return false;

        into.mPositions = arrays.mPositions;
        into.mNormals = arrays.mNormals;
        into.mIndices = mFold.getIndices();

        into.mTexCoords = {};
        const osg::Vec2Array* texCoords = asVec2Array(geometry.getTexCoordArray(0));
        if (texCoords != nullptr && texCoords->size() == arrays.mPositions.size())
            into.mTexCoords = std::span(texCoords->asVector());

        return true;
    }
}
