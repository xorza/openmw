#include <cstddef>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/Vec3f>

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

            EXPECT_EQ(reading.mPositions.size(), 4u);
            EXPECT_EQ(reading.mIndices.size(), 6u) << "two triangles, none of them the other's reverse";
            EXPECT_FALSE(reading.mShape.mSheet);
            EXPECT_TRUE(reading.mTexCoords.empty()) << "the quad carries none";

            ASSERT_EQ(reading.mNormals.size(), 4u) << "one normal for the whole drawable is a normal at every vertex";
            for (const osg::Vec3f& normal : reading.mNormals)
                EXPECT_EQ(normal, osg::Vec3f(0.0f, 0.0f, 1.0f));

            // A drawable with no triangles mirrors nothing, and says so rather than reading zero.
            osg::ref_ptr<osg::Geometry> empty = new osg::Geometry;
            EXPECT_FALSE(reader.read(readDrawable(*empty), reading));
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
            Known& entry = adopted.mResolver.adopt(*quad, reading, sNoIndex);
            EXPECT_EQ(entry.mIndex, 0u);
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

            // And adopting again stamps rather than adds, returning the same entry.
            EXPECT_EQ(&adopted.mResolver.adopt(*quad, reading, sNoIndex), &entry);
            EXPECT_EQ(adopted.mStats.mMeshesAdded, 1u);
        }
    }
}
