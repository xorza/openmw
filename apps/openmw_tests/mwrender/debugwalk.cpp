#include <vector>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/PrimitiveSet>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <apps/openmw/mwrender/rtx/debugwalk.hpp>
#include <apps/openmw/mwrender/vismask.hpp>
#include <components/rtx/debuglines.hpp>

namespace MWRender
{
    namespace
    {
        /// A geometry of `positions`, painted `colours` per vertex where there are as many, or the
        /// first over the whole where there is one, drawn as `mode`.
        osg::ref_ptr<osg::Geometry> drawn(
            const std::vector<osg::Vec3f>& positions, const std::vector<osg::Vec4f>& colours, const GLenum mode)
        {
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(new osg::Vec3Array(positions.begin(), positions.end()));
            geometry->setColorArray(new osg::Vec4Array(colours.begin(), colours.end()),
                colours.size() == positions.size() ? osg::Array::BIND_PER_VERTEX : osg::Array::BIND_OVERALL);
            geometry->addPrimitiveSet(new osg::DrawArrays(mode, 0, static_cast<int>(positions.size())));
            return geometry;
        }

        /// The walk reads every debug geometry under the root by the transform in force at it,
        /// takes a strip and a quad apart into lines and triangles, keeps each vertex's own colour
        /// or the one over the whole, and enters nothing that is not a debug node.
        ///
        /// **`Mask_Debug` and no other**, which is what the rasterizer's cull draws them by: a
        /// scene root beside the debug nodes holds the world, and a walk that entered it would read
        /// every mesh in the cell as a line drawing.
        TEST(RtxDebugWalkTest, theWalkReadsDebugGeometryByItsTransformAndNothingElse)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;

            // A pathgrid's lines, moved a hundred along x, under the debug mask.
            osg::ref_ptr<osg::MatrixTransform> moved
                = new osg::MatrixTransform(osg::Matrix::translate(100.0, 0.0, 0.0));
            moved->setNodeMask(Mask_Debug);
            moved->addChild(
                drawn({ osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 10.0f, 0.0f), osg::Vec3f(0.0f, 20.0f, 0.0f) },
                    { osg::Vec4f(1.0f, 0.0f, 0.0f, 1.0f), osg::Vec4f(0.0f, 1.0f, 0.0f, 1.0f),
                        osg::Vec4f(0.0f, 0.0f, 1.0f, 1.0f) },
                    GL_LINE_STRIP));
            root->addChild(moved);

            // A navmesh's quad, one colour over the whole, straight under the root.
            osg::ref_ptr<osg::Geometry> quad = drawn({ osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f),
                                                         osg::Vec3f(1.0f, 1.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f) },
                { osg::Vec4f(0.5f, 0.5f, 0.5f, 0.25f) }, GL_QUADS);
            quad->setNodeMask(Mask_Debug);
            root->addChild(quad);

            // The scene, which is not a debug node and is not read.
            osg::ref_ptr<osg::Geometry> scene = drawn(
                { osg::Vec3f(), osg::Vec3f(), osg::Vec3f() }, { osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f) }, GL_TRIANGLES);
            scene->setNodeMask(Mask_Scene);
            root->addChild(scene);

            DebugWalk walk;
            const Rtx::DebugLines lines = walk.walk(*root);

            // A strip of three is two lines of two vertices, each where the transform put it and
            // each its own colour.
            ASSERT_EQ(lines.mLines.size(), 4u);
            EXPECT_EQ(lines.mLines[0].mPosition, osg::Vec3f(100.0f, 0.0f, 0.0f));
            EXPECT_EQ(lines.mLines[1].mPosition, osg::Vec3f(100.0f, 10.0f, 0.0f));
            EXPECT_EQ(lines.mLines[2].mPosition, osg::Vec3f(100.0f, 10.0f, 0.0f));
            EXPECT_EQ(lines.mLines[3].mPosition, osg::Vec3f(100.0f, 20.0f, 0.0f));
            EXPECT_EQ(lines.mLines[0].mColour, osg::Vec4f(1.0f, 0.0f, 0.0f, 1.0f));
            EXPECT_EQ(lines.mLines[1].mColour, osg::Vec4f(0.0f, 1.0f, 0.0f, 1.0f));
            EXPECT_EQ(lines.mLines[3].mColour, osg::Vec4f(0.0f, 0.0f, 1.0f, 1.0f));

            // A quad is two triangles, and the scene's triangle is not among them.
            ASSERT_EQ(lines.mTriangles.size(), 6u);
            EXPECT_EQ(lines.mTriangles[0].mPosition, osg::Vec3f(0.0f, 0.0f, 0.0f));
            EXPECT_EQ(lines.mTriangles[2].mPosition, osg::Vec3f(1.0f, 1.0f, 0.0f));
            EXPECT_EQ(lines.mTriangles[5].mPosition, osg::Vec3f(0.0f, 1.0f, 0.0f));
            for (const Rtx::DebugVertex& vertex : lines.mTriangles)
                EXPECT_EQ(vertex.mColour, osg::Vec4f(0.5f, 0.5f, 0.5f, 0.25f));

            // And the next walk starts over rather than adding to the last.
            const Rtx::DebugLines again = walk.walk(*root);
            EXPECT_EQ(again.mLines.size(), 4u);
            EXPECT_EQ(again.mTriangles.size(), 6u);

            // With no debug node under it, a root answers nothing.
            osg::ref_ptr<osg::Group> plain = new osg::Group;
            plain->addChild(scene);
            EXPECT_TRUE(walk.walk(*plain).empty());
        }
    }
}
