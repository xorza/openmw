#include <cstddef>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/Vec4ub>

#include <components/rtx/extractionstats.hpp>
#include <components/rtx/index.hpp>
#include <components/rtx/meshreader.hpp>
#include <components/rtx/meshresolver.hpp>
#include <components/rtx/mirrorpass.hpp>
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
            ASSERT_TRUE(reader.read(readDrawable(*quad), reading));

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
            EXPECT_FALSE(reader.read(readDrawable(*empty), reading));
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

            ASSERT_TRUE(reader.read(readDrawable(*floats), reading));
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

            ASSERT_TRUE(reader.read(readDrawable(*bytes), reading));
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

            ASSERT_TRUE(reader.read(readDrawable(*overall), reading));
            ASSERT_EQ(reading.mArrays.mColours.size(), 4u);
            for (const osg::Vec3f& colour : reading.mArrays.mColours)
            {
                EXPECT_FLOAT_EQ(colour.x(), 1.0f);
                EXPECT_FLOAT_EQ(colour.y(), sAt128);
                EXPECT_FLOAT_EQ(colour.z(), 0.0f);
            }

            // An array of another length is a content file this cannot match up, and is left out
            // rather than read against the wrong vertices.
            osg::ref_ptr<osg::Geometry> mismatched = makeQuad();
            osg::ref_ptr<osg::Vec4Array> two = new osg::Vec4Array;
            two->push_back(osg::Vec4f(1.0f, 0.0f, 0.0f, 1.0f));
            two->push_back(osg::Vec4f(0.0f, 1.0f, 0.0f, 1.0f));
            mismatched->setColorArray(two, osg::Array::BIND_PER_VERTEX);

            ASSERT_TRUE(reader.read(readDrawable(*mismatched), reading));
            EXPECT_TRUE(reading.mArrays.mColours.empty());
        }

        /// The two halves land on one row: a mesh adopted from a reading is the mesh `resolve`
        /// would have added, and the drawable resolves to it afterwards rather than to a copy.
        TEST(RtxMeshReaderTest, anAdoptedReadingIsTheMeshResolveWouldHaveAddedAndResolveFindsIt)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();

            MeshReader reader;
            MeshReading reading;
            ASSERT_TRUE(reader.read(readDrawable(*quad), reading));

            Resolving adopted;
            const Index mesh = adopted.mResolver.adopt(*quad, reading, sNoIndex);
            EXPECT_EQ(mesh, 0u);
            EXPECT_EQ(adopted.mStats.mMeshesAdded, 1u);

            Resolving resolved;
            EXPECT_EQ(resolved.mResolver.resolve(*quad, readDrawable(*quad), sNoIndex), 0u);

            const SceneTables left = adopted.mScene.getTables();
            const SceneTables right = resolved.mScene.getTables();
            EXPECT_EQ(left.mMeshes.getRows()[0].mVertices.mCount, right.mMeshes.getRows()[0].mVertices.mCount);
            EXPECT_EQ(left.mMeshes.getRows()[0].mIndices.mCount, right.mMeshes.getRows()[0].mIndices.mCount);
            EXPECT_EQ(left.mMeshes.getRows()[0].mDeform, Deform::None);
            EXPECT_EQ(std::vector(left.mMeshes.getPositions().begin(), left.mMeshes.getPositions().end()),
                std::vector(right.mMeshes.getPositions().begin(), right.mMeshes.getPositions().end()));
            EXPECT_EQ(std::vector(left.mMeshes.getIndices().begin(), left.mMeshes.getIndices().end()),
                std::vector(right.mMeshes.getIndices().begin(), right.mMeshes.getIndices().end()));

            // The walk meeting the drawable after the ring adopted it finds the ring's mesh.
            EXPECT_EQ(adopted.mResolver.resolve(*quad, readDrawable(*quad), sNoIndex), 0u);
            EXPECT_EQ(adopted.mStats.mMeshesAdded, 1u) << "resolved to the mesh already held";
            EXPECT_EQ(adopted.mStats.mMeshesReused, 1u);

            // And adopting again holds rather than adds, answering the same mesh — and a hold is what
            // keeps the entry through a sweep the walk did not stamp it in.
            EXPECT_EQ(adopted.mResolver.adopt(*quad, reading, sNoIndex), mesh);
            EXPECT_EQ(adopted.mStats.mMeshesAdded, 1u);
            EXPECT_TRUE(adopted.mResolver.whole());

            adopted.mResolver.release(*quad);
            adopted.mResolver.release(*quad);
        }
    }
}
