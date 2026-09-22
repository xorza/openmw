#include "meshreader.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include <osg/Array>
#include <osg/Drawable>
#include <osg/Geometry>
#include <osg/TriangleIndexFunctor>

#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>

#include "colour.hpp"
#include "framespend.hpp"

namespace Rtx
{
    namespace
    {
        struct TriangleCollector
        {
            std::vector<std::uint32_t>* mIndices = nullptr;

            /// The largest index collected, which `NifOsg` hands on as the file wrote it: nothing
            /// between the file and here holds a triangle to the vertices it has.
            std::uint32_t mLargest = 0;

            void operator()(unsigned int a, unsigned int b, unsigned int c)
            {
                if (a == b || b == c || a == c)
                    return;

                mLargest = std::max({ mLargest, a, b, c });
                mIndices->push_back(a);
                mIndices->push_back(b);
                mIndices->push_back(c);
            }
        };

        struct VertexArrays
        {
            std::span<const osg::Vec3f> mPositions;

            /// Empty only where the geometry names no normal at all. A per-vertex array is taken as
            /// it stands and a single overall one is spread across the vertices, which is the same
            /// answer at every point of a flat surface.
            std::span<const osg::Vec3f> mNormals;
        };

        /// The array as a `Vec3Array`, or null where it is anything else. `osg::Array` states its
        /// own type in a byte, which is what a `dynamic_cast` walks the class hierarchy to work out.
        const osg::Vec3Array* asVec3Array(const osg::Array* array)
        {
            if (array == nullptr || array->getType() != osg::Array::Vec3ArrayType)
                return nullptr;

            return static_cast<const osg::Vec3Array*>(array);
        }

        /// A geometry's `what` array of `named` elements, against its `vertices`: an array of another
        /// length is one this cannot match to the vertices, and a guess at which belongs to which
        /// is a wrong picture rather than a missing one.
        Result<void, std::string> checkLength(std::string_view what, std::size_t named, std::size_t vertices)
        {
            if (named != vertices)
                return Err{ "it has " + std::to_string(named) + ' ' + std::string(what) + " for "
                    + std::to_string(vertices) + " vertices" };

            return {};
        }

        /// @param flat scratch for an overall normal spread across the vertices. Refilled here and
        ///        borrowed by the returned span, so it has to outlive the read.
        Result<VertexArrays, std::string> readVertices(const osg::Geometry& geometry, std::vector<osg::Vec3f>& flat)
        {
            VertexArrays arrays;

            // No array is no geometry, which the game draws nothing for either; an array of a type
            // this does not read is one it does.
            const osg::Array* vertices = geometry.getVertexArray();
            if (vertices == nullptr || vertices->getNumElements() == 0)
                return arrays;

            const osg::Vec3Array* positions = asVec3Array(vertices);
            if (positions == nullptr)
                return Err{ "its vertices are not three floats each" };

            arrays.mPositions = std::span(positions->asVector());

            const osg::Array* named = geometry.getNormalArray();
            if (named == nullptr || named->getNumElements() == 0)
                return arrays;

            const osg::Vec3Array* normals = asVec3Array(named);
            if (normals == nullptr)
                return Err{ "its normals are not three floats each" };

            // One normal for the whole drawable is a normal: `SceneUtil::createWaterGeometry` binds
            // exactly this, and dropping it made the sea flat black and took the exposure with it.
            if (normals->size() != positions->size() && normals->getBinding() == osg::Array::BIND_OVERALL)
            {
                flat.assign(positions->size(), normals->at(0));
                arrays.mNormals = std::span(flat);
                return arrays;
            }

            if (const Result<void, std::string> matched = checkLength("normals", normals->size(), positions->size());
                !matched.isOk())
                return Err{ matched.error() };

            arrays.mNormals = std::span(normals->asVector());
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

        /// The coordinates bound at `unit`, or null where none are. An error where they are of a
        /// type this does not read, or where they do not match the vertices — `checkLength`.
        Result<const osg::Vec2Array*, std::string> readTexCoords(
            const osg::Geometry& geometry, unsigned int unit, std::size_t vertices)
        {
            const osg::Array* named = geometry.getTexCoordArray(unit);
            if (named == nullptr || named->getNumElements() == 0)
                return nullptr;

            const osg::Vec2Array* coords = asVec2Array(named);
            if (coords == nullptr)
                return Err{ "its texture coordinates are not two floats each" };

            if (const Result<void, std::string> matched = checkLength("texture coordinates", coords->size(), vertices);
                !matched.isOk())
                return Err{ matched.error() };

            return coords;
        }

        /// A geometry's per-vertex colours, decoded into `scratch` and spanned from it. Empty where
        /// the geometry names none. Two array types, because `NifOsg` builds a `Vec4Array` from a
        /// `NiGeometryData` and a `Vec4ubArray` from a `BSTriShape`. An overall colour is spread
        /// across the vertices, as `readVertices` spreads an overall normal. The alpha is not
        /// read: three shapes in the whole of vanilla carry one below opaque, and reading it would
        /// put a fetch on every candidate of every shadow ray.
        ///
        /// @param vertices how many the geometry holds, which an array that is not overall has to
        ///        match — `checkLength`.
        Result<std::span<const osg::Vec3f>, std::string> readColours(
            const osg::Geometry& geometry, const std::size_t vertices, std::vector<osg::Vec3f>& scratch)
        {
            const osg::Array* colours = geometry.getColorArray();
            if (colours == nullptr || colours->getNumElements() == 0)
                return std::span<const osg::Vec3f>();

            const bool overall = colours->getBinding() == osg::Array::BIND_OVERALL;
            if (!overall)
            {
                const Result<void, std::string> matched = checkLength("colours", colours->getNumElements(), vertices);
                if (!matched.isOk())
                    return Err{ matched.error() };
            }

            const auto decodeAll = [&](const auto& array) {
                if (overall)
                {
                    scratch.assign(vertices, decodeColour(array[0]));
                    return;
                }

                scratch.clear();
                scratch.reserve(vertices);
                for (std::size_t at = 0; at < vertices; ++at)
                    scratch.push_back(decodeColour(array[at]));
            };

            switch (colours->getType())
            {
                case osg::Array::Vec4ArrayType:
                    decodeAll(static_cast<const osg::Vec4Array&>(*colours));
                    break;
                case osg::Array::Vec4ubArrayType:
                    decodeAll(static_cast<const osg::Vec4ubArray&>(*colours));
                    break;
                default:
                    return Err{ "its colours are neither four floats nor four bytes each" };
            }

            return std::span<const osg::Vec3f>(scratch);
        }
    }

    DrawableRead readDrawable(const osg::Drawable& drawable, const NodeKind kind)
    {
        if (const osg::Geometry* geometry = drawable.asGeometry())
            return DrawableRead{ .mGeometry = geometry };

        if (const auto* rig = as<const SceneUtil::RigGeometry>(kind, NodeKind::RigGeometry, drawable))
        {
            const bool skinned = rig->getInfluenceData() != nullptr && !rig->getBones().empty();
            return DrawableRead{ .mGeometry = rig->getSourceGeometry().get(),
                .mDeform = skinned ? Deform::Rig : Deform::None,
                .mRig = rig };
        }

        if (const auto* morph = as<const SceneUtil::MorphGeometry>(kind, NodeKind::MorphGeometry, drawable))
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

    Result<bool, std::string> MeshReader::collectTriangles(const osg::Geometry& geometry, const std::size_t vertices)
    {
        mIndexScratch.clear();

        osg::TriangleIndexFunctor<TriangleCollector> collector;
        collector.mIndices = &mIndexScratch;
        geometry.accept(collector);

        if (mIndexScratch.empty())
            return false;

        if (collector.mLargest >= vertices)
            return Err{ "its triangles name vertex " + std::to_string(collector.mLargest) + " of "
                + std::to_string(vertices) };

        return true;
    }

    Result<bool, std::string> MeshReader::read(const DrawableRead& read, MeshReading& into)
    {
        const osg::Geometry& geometry = *read.mGeometry;

        const Result<VertexArrays, std::string> vertices = readVertices(geometry, mFlatNormalScratch);
        if (!vertices.isOk())
            return Err{ vertices.error() };

        VertexArrays arrays = vertices.value();

        // A morph starts from its base target and not from the source's array, because that is
        // what `MorphGeometry::cull` starts from. The normals and everything else are the source's.
        if (read.mDeform == Deform::Morph)
        {
            const std::span<const osg::Vec3f> base = morphBase(*read.mMorph);
            if (base.size() != arrays.mPositions.size())
                return Err{ "its base target has " + std::to_string(base.size()) + " vertices for "
                    + std::to_string(arrays.mPositions.size()) };

            arrays.mPositions = base;
        }

        if (arrays.mPositions.empty())
            return false;

        const std::size_t count = arrays.mPositions.size();

        // Every array asked before the fold, so a face this refuses costs no fold.
        const Result<const osg::Vec2Array*, std::string> texCoords = readTexCoords(geometry, 0, count);
        if (!texCoords.isOk())
            return Err{ texCoords.error() };

        // **A second set is the first array bound at any unit that is not unit nought's**, and
        // the units that bind it are noted for the material to look up its dark map's stream by.
        // `NifOsg` binds a shape's UV sets one per texture unit, in the order the texturing
        // property names them, so a unit that reads another array than unit nought's is reading
        // the shape's second set. No vanilla shape carries a third.
        const osg::Vec2Array* second = nullptr;
        std::uint32_t unitStreams = 0;
        for (unsigned int unit = 1; unit < geometry.getNumTexCoordArrays() && unit < 32; ++unit)
        {
            const Result<const osg::Vec2Array*, std::string> bound = readTexCoords(geometry, unit, count);
            if (!bound.isOk())
                return Err{ bound.error() };
            if (bound.value() == nullptr || bound.value() == texCoords.value())
                continue;
            if (second == nullptr)
                second = bound.value();
            if (bound.value() == second)
                unitStreams |= 1u << unit;
        }

        // The source geometry's colours even for a morph, whose positions came from its base
        // target: a morph moves vertices and does not repaint them.
        const Result<std::span<const osg::Vec3f>, std::string> colours = readColours(geometry, count, mColourScratch);
        if (!colours.isOk())
            return Err{ colours.error() };

        // Folded before the mesh is written, so the copy the content drew for a card's back never
        // reaches a structure. Once per drawable and never for a pose: a rig moves the two copies
        // together, so the pairs found in the bind pose are the pairs.
        const std::chrono::steady_clock::time_point folding = std::chrono::steady_clock::now();
        const Result<bool, std::string> collected = collectTriangles(geometry, count);
        if (!collected.isOk())
            return Err{ collected.error() };
        if (collected.value())
            into.mShape = mFold.fold(arrays.mPositions, mIndexScratch);
        into.mFoldMs = since(folding, std::chrono::steady_clock::now());

        if (!collected.value())
            return false;

        into.mArrays = MeshArrays{
            .mPositions = arrays.mPositions,
            .mNormals = arrays.mNormals,
            .mTexCoords
            = texCoords.value() != nullptr ? std::span(texCoords.value()->asVector()) : std::span<const osg::Vec2f>(),
            .mSecondTexCoords = second != nullptr ? std::span(second->asVector()) : std::span<const osg::Vec2f>(),
            .mUnitStreams = unitStreams,
            .mColours = colours.value(),
            .mIndices = mIndexScratch,
        };

        return true;
    }
}
