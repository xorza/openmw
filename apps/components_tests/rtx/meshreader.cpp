#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/Vec4ub>
#include <osg/ref_ptr>

#include <components/rtx/extractionstats.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/meshreader.hpp>
#include <components/rtx/meshresolver.hpp>
#include <components/rtx/mirrorpass.hpp>
#include <components/rtx/nodekind.hpp>
#include <components/rtx/result.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>

#include "extractor/fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// A resolver on a scene of its own, inside a pass, which is what `MirrorPass::getStats`
        /// insists on.
        struct Resolving
        {
            SceneDesc mScene;
            ExtractionStats mStats;
            MirrorPass mPass;
            MeshResolver mResolver{ mScene, mPass };

            Resolving() { mPass.mStats = &mStats; }
        };

        TEST(RtxMeshReaderTest, aReadingIsWhatTheGeometryHoldsFoldedAndAnOverallNormalIsSpread)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();

            osg::ref_ptr<osg::Vec3Array> overall = new osg::Vec3Array;
            overall->push_back(osg::Vec3f(0.0f, 0.0f, 1.0f));
            quad->setNormalArray(overall, osg::Array::BIND_OVERALL);

            MeshReader reader;
            MeshReading reading;
            ASSERT_TRUE(reader.read(readDrawable(*quad, NodeKinds{}.of(*quad)), reading).value());

            EXPECT_EQ(reading.mArrays.mPositions.size(), 4u);
            EXPECT_EQ(reading.mArrays.mIndices.size(), 6u) << "two triangles, none of them the other's reverse";
            EXPECT_FALSE(reading.mShape.mSheet);
            EXPECT_TRUE(reading.mArrays.mTexCoords.empty()) << "the quad carries none";
            EXPECT_TRUE(reading.mArrays.mColours.empty()) << "nor any colour";

            ASSERT_EQ(reading.mArrays.mNormals.size(), 4u)
                << "one normal for the whole drawable is a normal at every vertex";
            for (const osg::Vec3f& normal : reading.mArrays.mNormals)
                EXPECT_EQ(normal, osg::Vec3f(0.0f, 0.0f, 1.0f));

            // A drawable with no triangles mirrors nothing, and says so rather than reading zero.
            osg::ref_ptr<osg::Geometry> empty = new osg::Geometry;
            EXPECT_FALSE(reader.read(readDrawable(*empty, NodeKinds{}.of(*empty)), reading).value());

            // One whose triangles name a vertex it does not have is refused by name: four vertices,
            // and the second triangle ends at index four.
            osg::ref_ptr<osg::Geometry> past = makeIndexPastItsVertices();
            const Result<bool, std::string> refused = reader.read(readDrawable(*past, NodeKinds{}.of(*past)), reading);
            ASSERT_FALSE(refused.isOk()) << "a triangle past its vertices was read";
            EXPECT_EQ(refused.error(), "its triangles name vertex 4 of 4");
        }

        /// The colours are decoded on the way in, whichever of the two arrays the loader built.
        ///
        /// **The values are the sRGB curve's own**, worked out here rather than asked of
        /// `Rtx::toLinear`: a test that asked the decoder what the decoder answers would pass
        /// against any curve at all. `(k / 255 + 0.055) / 1.055` raised to 2.4, which for 64, 128
        /// and 255 is 0.05126946, 0.21586050 and 1.
        TEST(RtxMeshReaderTest, coloursAreDecodedToLightFromEitherArrayTheLoaderBuilds)
        {
            constexpr float sAt64 = 0.05126946f;
            constexpr float sAt128 = 0.21586050f;

            MeshReader reader;
            MeshReading reading;

            // What `NifOsg` builds from a `NiGeometryData`: four floats a vertex.
            osg::ref_ptr<osg::Geometry> floats = makeQuad();
            osg::ref_ptr<osg::Vec4Array> asFloats = new osg::Vec4Array;
            for (int at = 0; at < 4; ++at)
                asFloats->push_back(osg::Vec4f(64.0f / 255.0f, 128.0f / 255.0f, 1.0f, 1.0f));
            floats->setColorArray(asFloats, osg::Array::BIND_PER_VERTEX);

            ASSERT_TRUE(reader.read(readDrawable(*floats, NodeKinds{}.of(*floats)), reading).value());
            ASSERT_EQ(reading.mArrays.mColours.size(), 4u);
            for (const osg::Vec3f& colour : reading.mArrays.mColours)
            {
                EXPECT_FLOAT_EQ(colour.x(), sAt64);
                EXPECT_FLOAT_EQ(colour.y(), sAt128);
                EXPECT_FLOAT_EQ(colour.z(), 1.0f) << "the byte the curve leaves alone";
            }

            // What a `BSTriShape` and `Terrain::Storage` build: one byte a channel.
            osg::ref_ptr<osg::Geometry> bytes = makeQuad();
            osg::ref_ptr<osg::Vec4ubArray> asBytes = new osg::Vec4ubArray;
            for (int at = 0; at < 4; ++at)
                asBytes->push_back(osg::Vec4ub(64, 128, 255, 255));
            bytes->setColorArray(asBytes, osg::Array::BIND_PER_VERTEX);

            ASSERT_TRUE(reader.read(readDrawable(*bytes, NodeKinds{}.of(*bytes)), reading).value());
            ASSERT_EQ(reading.mArrays.mColours.size(), 4u);
            for (const osg::Vec3f& colour : reading.mArrays.mColours)
            {
                EXPECT_FLOAT_EQ(colour.x(), sAt64) << "the two arrays decode to one answer";
                EXPECT_FLOAT_EQ(colour.y(), sAt128);
                EXPECT_FLOAT_EQ(colour.z(), 1.0f);
            }

            // One colour for the whole drawable is a colour at every vertex, as an overall normal
            // is a normal at every vertex.
            osg::ref_ptr<osg::Geometry> overall = makeQuad();
            osg::ref_ptr<osg::Vec4Array> one = new osg::Vec4Array;
            one->push_back(osg::Vec4f(1.0f, 128.0f / 255.0f, 0.0f, 1.0f));
            overall->setColorArray(one, osg::Array::BIND_OVERALL);

            ASSERT_TRUE(reader.read(readDrawable(*overall, NodeKinds{}.of(*overall)), reading).value());
            ASSERT_EQ(reading.mArrays.mColours.size(), 4u);
            for (const osg::Vec3f& colour : reading.mArrays.mColours)
            {
                EXPECT_FLOAT_EQ(colour.x(), 1.0f);
                EXPECT_FLOAT_EQ(colour.y(), sAt128);
                EXPECT_FLOAT_EQ(colour.z(), 0.0f);
            }
        }

        /// An array this cannot match to the vertices refuses the whole face, and says which: one of
        /// another length, or one of a type this does not read. Read against the wrong vertices, or
        /// left out, it would be a picture of something the content did not describe — a face lit
        /// by the wrong normals, or one drawn untextured.
        TEST(RtxMeshReaderTest, anArrayThatDoesNotMatchTheVerticesRefusesTheFaceAndSaysWhich)
        {
            const auto pairs = [](std::size_t count) {
                osg::ref_ptr<osg::Vec2Array> array = new osg::Vec2Array(static_cast<unsigned int>(count));
                return array;
            };

            osg::ref_ptr<osg::Geometry> shortNormals = makeQuad();
            shortNormals->setNormalArray(
                makePositions({ osg::Vec3f(0, 0, 1), osg::Vec3f(0, 0, 1), osg::Vec3f(0, 0, 1) }),
                osg::Array::BIND_PER_VERTEX);

            osg::ref_ptr<osg::Geometry> doubleNormals = makeQuad();
            doubleNormals->setNormalArray(new osg::Vec3dArray(4), osg::Array::BIND_PER_VERTEX);

            osg::ref_ptr<osg::Geometry> shortCoords = makeQuad();
            shortCoords->setTexCoordArray(0, pairs(2));

            osg::ref_ptr<osg::Geometry> tripleCoords = makeQuad();
            tripleCoords->setTexCoordArray(0, new osg::Vec3Array(4));

            osg::ref_ptr<osg::Geometry> shortSecond = makeQuad();
            shortSecond->setTexCoordArray(0, pairs(4));
            shortSecond->setTexCoordArray(1, pairs(3));

            osg::ref_ptr<osg::Vec4Array> two = new osg::Vec4Array(2);
            osg::ref_ptr<osg::Geometry> shortColours = makeQuad();
            shortColours->setColorArray(two, osg::Array::BIND_PER_VERTEX);

            osg::ref_ptr<osg::Geometry> tripleColours = makeQuad();
            tripleColours->setColorArray(new osg::Vec3Array(4), osg::Array::BIND_PER_VERTEX);

            const std::array<std::pair<osg::ref_ptr<osg::Geometry>, std::string_view>, 7> broken{ {
                { shortNormals, "it has 3 normals for 4 vertices" },
                { doubleNormals, "its normals are not three floats each" },
                { shortCoords, "it has 2 texture coordinates for 4 vertices" },
                { tripleCoords, "its texture coordinates are not two floats each" },
                { shortSecond, "it has 3 texture coordinates for 4 vertices" },
                { shortColours, "it has 2 colours for 4 vertices" },
                { tripleColours, "its colours are neither four floats nor four bytes each" },
            } };

            MeshReader reader;
            MeshReading reading;
            for (const auto& [geometry, why] : broken)
            {
                const Result<bool, std::string> refused
                    = reader.read(readDrawable(*geometry, NodeKinds{}.of(*geometry)), reading);
                ASSERT_FALSE(refused.isOk()) << "read a face that should be refused because " << why;
                EXPECT_EQ(refused.error(), why);
            }

            // And the same quad with every array matching is read, so what refused each of those is
            // the array and not the quad.
            osg::ref_ptr<osg::Geometry> whole = makeQuad();
            whole->setNormalArray(new osg::Vec3Array(4), osg::Array::BIND_PER_VERTEX);
            whole->setTexCoordArray(0, pairs(4));
            whole->setTexCoordArray(1, pairs(4));
            whole->setColorArray(new osg::Vec4Array(4), osg::Array::BIND_PER_VERTEX);
            EXPECT_TRUE(reader.read(readDrawable(*whole, NodeKinds{}.of(*whole)), reading).value());
        }

        /// The two halves land on one row: a mesh adopted from a reading is the mesh `resolve`
        /// would have added, and the drawable resolves to it afterwards rather than to a copy.
        TEST(RtxMeshReaderTest, anAdoptedReadingIsTheMeshResolveWouldHaveAddedAndResolveFindsIt)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();

            MeshReader reader;
            MeshReading reading;
            ASSERT_TRUE(reader.read(readDrawable(*quad, NodeKinds{}.of(*quad)), reading).value());

            Resolving adopted;
            const Index mesh = adopted.mResolver.adopt(*quad, reading);
            EXPECT_EQ(mesh, 0u);
            EXPECT_EQ(adopted.mStats.mMeshesAdded, 1u);

            Resolving resolved;
            EXPECT_EQ(resolved.mResolver.resolve(*quad, readDrawable(*quad, NodeKinds{}.of(*quad))), 0u);

            const SceneDesc& left = adopted.mScene;
            const SceneDesc& right = resolved.mScene;
            EXPECT_EQ(left.meshes().getRows()[0].mVertices.mCount, right.meshes().getRows()[0].mVertices.mCount);
            EXPECT_EQ(left.meshes().getRows()[0].mIndices.mCount, right.meshes().getRows()[0].mIndices.mCount);
            EXPECT_EQ(left.meshes().getRows()[0].mDeform, Deform::None);
            const std::span<const osg::Vec3f> leftPositions = left.meshes().getMeshPositions(0);
            const std::span<const osg::Vec3f> rightPositions = right.meshes().getMeshPositions(0);
            const std::span<const std::uint32_t> leftIndices = left.meshes().getMeshIndices(0);
            const std::span<const std::uint32_t> rightIndices = right.meshes().getMeshIndices(0);
            EXPECT_EQ(std::vector(leftPositions.begin(), leftPositions.end()),
                std::vector(rightPositions.begin(), rightPositions.end()));
            EXPECT_EQ(std::vector(leftIndices.begin(), leftIndices.end()),
                std::vector(rightIndices.begin(), rightIndices.end()));

            // The walk meeting the drawable after the ring adopted it finds the ring's mesh.
            EXPECT_EQ(adopted.mResolver.resolve(*quad, readDrawable(*quad, NodeKinds{}.of(*quad))), 0u);
            EXPECT_EQ(adopted.mStats.mMeshesAdded, 1u) << "resolved to the mesh already held";
            EXPECT_EQ(adopted.mStats.mMeshesReused, 1u);

            // And adopting again holds rather than adds, answering the same mesh — and a hold is what
            // keeps the entry through a sweep the walk did not stamp it in.
            EXPECT_EQ(adopted.mResolver.adopt(*quad, reading), mesh);
            EXPECT_EQ(adopted.mStats.mMeshesAdded, 1u);
            EXPECT_TRUE(adopted.mResolver.whole());

            adopted.mResolver.release(*quad);
            adopted.mResolver.release(*quad);
        }
    }
}
