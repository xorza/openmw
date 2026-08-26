#include <initializer_list>

#include <gtest/gtest.h>

#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/resource/imagemanager.hpp>
#include <components/rtx/offscreentrace.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/vfs/manager.hpp>

#include "countingrenderer.hpp"

namespace Rtx
{
    namespace
    {
        /// A unit quad in the xy plane: four vertices, two triangles.
        osg::ref_ptr<osg::Geometry> makeQuad()
        {
            osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
            for (const osg::Vec3f& value : { osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f),
                     osg::Vec3f(1.0f, 1.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f) })
                positions->push_back(value);

            osg::ref_ptr<osg::DrawElementsUInt> triangles = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);
            for (const unsigned int index : { 0u, 1u, 2u, 0u, 2u, 3u })
                triangles->push_back(index);

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(positions);
            geometry->addPrimitiveSet(triangles);
            return geometry;
        }

        /// The clock a redraw runs on, which has to read differently every time: everything skinned
        /// refuses to move for a traversal number it has already seen.
        osg::ref_ptr<osg::FrameStamp> stampAt(unsigned int frame)
        {
            osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
            stamp->setFrameNumber(frame);
            stamp->setSimulationTime(frame);
            stamp->setReferenceTime(frame);
            return stamp;
        }

        /// **Two kinds, and the constructor is which.** A picture of the world owns no scene, which
        /// is also the answer to what its traversal mask does: nothing, because there is no walk of
        /// its own to mask. The rasterizer's camera cull mask is the only reader of that number,
        /// which is why `MWRender::LocalMap`'s inclusion mask is not the weather bug again.
        TEST(RtxOffscreenTraceTest, aPictureOfTheWorldOwnsNoSceneAndOneOfASubjectDoes)
        {
            Testing::CountingRenderer renderer;
            osg::ref_ptr<osg::Group> subject = new osg::Group;
            subject->addChild(makeQuad());

            const OffscreenTrace world(renderer, 64, 64);
            EXPECT_TRUE(world.isOfWorld());
            EXPECT_EQ(world.getScene(), nullptr);
            EXPECT_EQ(renderer.mViewScenes, 0u);

            const OffscreenTrace doll(renderer, 64, 64, *subject, ~0u);
            EXPECT_FALSE(doll.isOfWorld());
            ASSERT_NE(doll.getScene(), nullptr);
            EXPECT_EQ(renderer.mViewScenes, 1u);
        }

        /// A subject taken apart and put back together places what is there now and lets the rest go.
        ///
        /// **The property `NpcAnimation::updateParts` needs and nothing covered.** Between one
        /// redraw and the next the game frees the body parts that changed and builds their
        /// replacements, so a mirror that kept what it met last time draws the clothes the character
        /// took off — and one that swept by renumbering would rebuild every acceleration structure
        /// in the doll on every frame of a slider drag.
        TEST(RtxOffscreenTraceTest, aRebuiltSubjectPlacesWhatArrivedAndHandsTheRoomToWhatComesNext)
        {
            VFS::Manager vfs;
            Resource::ImageManager images(&vfs, 0);
            Testing::CountingRenderer renderer;

            osg::ref_ptr<osg::Geometry> body = makeQuad();
            osg::ref_ptr<osg::Geometry> shirt = makeQuad();

            osg::ref_ptr<osg::Group> subject = new osg::Group;
            subject->addChild(body);
            subject->addChild(shirt);

            OffscreenTrace trace(renderer, 64, 64, *subject, ~0u);
            const SceneDesc& scene = *trace.getScene();

            ASSERT_TRUE(trace.rebuildSubject(*stampAt(1), 1, images));

            // Two quads of two triangles each: what a scene holding both looks like.
            EXPECT_EQ(scene.getPlacedCount(), 2u);
            EXPECT_EQ(scene.getTriangleCount(), 4u);

            // The shirt comes off and a hat goes on — one part replaced, not moved.
            subject->removeChild(shirt);
            osg::ref_ptr<osg::Geometry> hat = makeQuad();
            subject->addChild(hat);

            ASSERT_TRUE(trace.rebuildSubject(*stampAt(2), 2, images));

            // **Still two placements and not three**, which is half the assertion: the hat was
            // placed and the shirt was swept. A mirror that kept what it no longer meets reads
            // three here.
            EXPECT_EQ(scene.getPlacedCount(), 2u);

            // **And six triangles and not four**, which is the other half: a swept mesh is *freed*
            // rather than compacted away, so the shirt keeps its room in the index buffer. Four here
            // would mean the sweep closed the gap and renumbered every mesh above it.
            EXPECT_EQ(scene.getTriangleCount(), 6u);

            // The hat comes off in turn, and what replaces it takes the room the sweep is holding.
            subject->removeChild(hat);
            osg::ref_ptr<osg::Geometry> boots = makeQuad();
            subject->addChild(boots);

            ASSERT_TRUE(trace.rebuildSubject(*stampAt(3), 3, images));

            EXPECT_EQ(scene.getPlacedCount(), 2u);

            // **Six again and not eight**, which is what a freed slot is for: the boots fit where
            // the hat was and the buffer did not grow. Eight would be a doll that leaks a mesh per
            // change of clothes.
            EXPECT_EQ(scene.getTriangleCount(), 6u);

            // **Built from nothing exactly once**, across three redraws that each replaced a part.
            // This is what the sweep freeing rather than compacting buys: a race-creation slider
            // drag redraws the same subject sixty times a second, and every one of those after the
            // first appends to what is already on the device.
            EXPECT_EQ(renderer.mRebuilt, 1u);
        }

        /// A subject with nothing in it is a picture nobody should trace.
        ///
        /// **Because a doll is asked for before it is dressed.** The inventory opens on a subtree the
        /// animation has not filled yet, and tracing that leaves the widget holding a scene with no
        /// acceleration structure in it.
        TEST(RtxOffscreenTraceTest, anEmptySubjectSaysThereIsNothingToTrace)
        {
            VFS::Manager vfs;
            Resource::ImageManager images(&vfs, 0);
            Testing::CountingRenderer renderer;

            osg::ref_ptr<osg::Group> subject = new osg::Group;

            OffscreenTrace trace(renderer, 64, 64, *subject, ~0u);
            EXPECT_FALSE(trace.rebuildSubject(*stampAt(1), 1, images));
            EXPECT_EQ(trace.getScene()->getPlacedCount(), 0u);
        }

        /// The mask is an inclusion mask, AND-ed at every node — so a category left out of it is
        /// dropped wherever it appears below, which is the shape the weather bug had.
        TEST(RtxOffscreenTraceTest, theSubjectMaskKeepsTheWalkOutOfWhatItDoesNotName)
        {
            VFS::Manager vfs;
            Resource::ImageManager images(&vfs, 0);
            Testing::CountingRenderer renderer;

            constexpr osg::Node::NodeMask wanted = 1u << 3;
            constexpr osg::Node::NodeMask other = 1u << 4;

            osg::ref_ptr<osg::MatrixTransform> kept = new osg::MatrixTransform;
            kept->setNodeMask(wanted);
            kept->addChild(makeQuad());

            osg::ref_ptr<osg::MatrixTransform> skipped = new osg::MatrixTransform;
            skipped->setNodeMask(other);
            skipped->addChild(makeQuad());

            osg::ref_ptr<osg::Group> subject = new osg::Group;
            subject->addChild(kept);
            subject->addChild(skipped);

            OffscreenTrace trace(renderer, 64, 64, *subject, wanted);
            ASSERT_TRUE(trace.rebuildSubject(*stampAt(1), 1, images));

            // One of the two, and the same fixture with `wanted | other` would take both — which is
            // what says the mask is doing the choosing rather than the fixture.
            EXPECT_EQ(trace.getScene()->getPlacedCount(), 1u);

            OffscreenTrace both(renderer, 64, 64, *subject, wanted | other);
            ASSERT_TRUE(both.rebuildSubject(*stampAt(1), 1, images));
            EXPECT_EQ(both.getScene()->getPlacedCount(), 2u);
        }
    }
}
