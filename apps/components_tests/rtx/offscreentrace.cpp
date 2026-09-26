#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Node>
#include <osg/PrimitiveSet>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/rtx/offscreentrace.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/slot.hpp>
#include <components/rtx/surfaceview.hpp>
#include <components/rtx/viewscene.hpp>
#include <components/sceneutil/offscreenframing.hpp>

#include "countingrenderer.hpp"

namespace Rtx
{
    namespace
    {
        /// The walk mask that keeps every node: a node mask's default is all ones.
        constexpr osg::Node::NodeMask sEveryNode = ~0u;

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

            const OffscreenTrace world(
                renderer, ViewRequest{ .mWidth = 64, .mHeight = 64, .mRayMask = Shaders::MASK_EVERY_CLASS });
            EXPECT_TRUE(world.isOfWorld());
            EXPECT_EQ(world.getScene(), nullptr);
            EXPECT_EQ(renderer.mViewScenes, 0u);

            {
                const OffscreenTrace doll(renderer,
                    ViewRequest{ .mWidth = 64,
                        .mHeight = 64,
                        .mRayMask = Shaders::MASK_EVERY_CLASS,
                        .mSubject = subject.get(),
                        .mSubjectMask = sEveryNode });
                EXPECT_FALSE(doll.isOfWorld());
                ASSERT_NE(doll.getScene(), nullptr);
                EXPECT_EQ(renderer.mViewScenes, 1u);
                EXPECT_TRUE(renderer.mViewDropped.empty());
            }

            // The scene goes with the picture, once.
            EXPECT_EQ(renderer.mViewDropped, (std::vector<std::uint32_t>{ 0 }));
        }

        /// **A picture states none of what its sampling decides, whatever the profile says.** The
        /// renderer applies its own texture rules to a picture as to a frame (`Rtx::sampleFrame`),
        /// and a field a picture stated would be a second writer — which the sampling refuses. Two
        /// profiles apart, so a picture that copied the profile in would show here.
        TEST(RtxOffscreenTraceTest, aPictureLeavesItsSamplingToTheRenderer)
        {
            Testing::CountingRenderer renderer;
            OffscreenTrace world(renderer,
                ViewRequest{ .mWidth = 64,
                    .mHeight = 64,
                    .mRayMask = Shaders::MASK_EVERY_CLASS,
                    .mFraming = { .mProjection = SceneUtil::Perspective{ .mFieldOfView = 60.f } } });

            for (const auto& [delight, albedo] : { std::pair{ 0.25f, false }, std::pair{ 0.75f, true } })
            {
                renderer.mProfile.mDelight = delight;
                renderer.mProfile.mShow = albedo ? SurfaceView::Albedo : SurfaceView::Shaded;
                world.traceInto(GuiSlot::at(0), false);

                ASSERT_TRUE(renderer.mTraced.has_value());
                EXPECT_EQ(renderer.mTraced->mDelight, 0.0f);
                EXPECT_EQ(renderer.mTraced->mShow, 0u);
                EXPECT_EQ(renderer.mTraced->mCamera.mJitter, osg::Vec2f());
            }
        }

        /// **The slot is the handle's, and one handle gives it back.** Moved, the slot goes with the
        /// move and the emptied handle drops nothing; the one that holds it at the end drops it
        /// once. What `OffscreenTrace` paired by hand across a constructor and a destructor.
        TEST(RtxOffscreenTraceTest, aViewSceneGivesItsSlotBackOnceHoweverItIsMoved)
        {
            Testing::CountingRenderer renderer;
            {
                ViewScene taken(renderer);
                EXPECT_TRUE(taken.holds());
                EXPECT_EQ(taken.get(), SceneSlot::view(0));
                EXPECT_EQ(renderer.mViewScenes, 1u);

                ViewScene moved(std::move(taken));
                EXPECT_FALSE(taken.holds()) << "a moved-from handle still holds the slot";
                EXPECT_EQ(moved.get(), SceneSlot::view(0));

                ViewScene assigned;
                EXPECT_FALSE(assigned.holds());
                assigned = std::move(moved);
                EXPECT_FALSE(moved.holds());
                EXPECT_EQ(assigned.get(), SceneSlot::view(0));
                EXPECT_TRUE(renderer.mViewDropped.empty()) << "a move dropped the slot";

                // Assigned over, the slot a handle held goes back before it takes the next.
                assigned = ViewScene(renderer);
                EXPECT_EQ(renderer.mViewDropped, (std::vector<std::uint32_t>{ 0 }));
                EXPECT_EQ(assigned.get(), SceneSlot::view(1));
            }
            EXPECT_EQ(renderer.mViewDropped, (std::vector<std::uint32_t>{ 0, 1 }));
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
            Testing::CountingRenderer renderer;

            osg::ref_ptr<osg::Geometry> body = makeQuad();
            osg::ref_ptr<osg::Geometry> shirt = makeQuad();

            osg::ref_ptr<osg::Group> subject = new osg::Group;
            subject->addChild(body);
            subject->addChild(shirt);

            OffscreenTrace trace(renderer,
                ViewRequest{ .mWidth = 64,
                    .mHeight = 64,
                    .mRayMask = Shaders::MASK_EVERY_CLASS,
                    .mSubject = subject.get(),
                    .mSubjectMask = sEveryNode });
            const SceneDesc& scene = *trace.getScene();

            ASSERT_TRUE(trace.rebuildSubject(*stampAt(1)));

            // Two quads of two triangles each: what a scene holding both looks like.
            EXPECT_EQ(scene.placements().getCounts().mPlaced, 2u);
            EXPECT_EQ(scene.meshes().getTriangleCount(), 4u);

            // The shirt comes off and a hat goes on — one part replaced, not moved.
            subject->removeChild(shirt);
            osg::ref_ptr<osg::Geometry> hat = makeQuad();
            subject->addChild(hat);

            ASSERT_TRUE(trace.rebuildSubject(*stampAt(2)));

            // **Still two placements and not three**, which is half the assertion: the hat was
            // placed and the shirt was swept. A mirror that kept what it no longer meets reads
            // three here.
            EXPECT_EQ(scene.placements().getCounts().mPlaced, 2u);

            // **And six triangles and not four**, which is the other half: a swept mesh is *freed*
            // rather than compacted away, so the shirt keeps its room in the index buffer. Four here
            // would mean the sweep closed the gap and renumbered every mesh above it.
            EXPECT_EQ(scene.meshes().getTriangleCount(), 6u);

            // The hat comes off in turn, and what replaces it takes the room the sweep is holding.
            subject->removeChild(hat);
            osg::ref_ptr<osg::Geometry> boots = makeQuad();
            subject->addChild(boots);

            ASSERT_TRUE(trace.rebuildSubject(*stampAt(3)));

            EXPECT_EQ(scene.placements().getCounts().mPlaced, 2u);

            // **Six again and not eight**, which is what a freed slot is for: the boots fit where
            // the hat was and the buffer did not grow. Eight would be a doll that leaks a mesh per
            // change of clothes.
            EXPECT_EQ(scene.meshes().getTriangleCount(), 6u);

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
            Testing::CountingRenderer renderer;

            osg::ref_ptr<osg::Group> subject = new osg::Group;

            OffscreenTrace trace(renderer,
                ViewRequest{ .mWidth = 64,
                    .mHeight = 64,
                    .mRayMask = Shaders::MASK_EVERY_CLASS,
                    .mSubject = subject.get(),
                    .mSubjectMask = sEveryNode });
            EXPECT_FALSE(trace.rebuildSubject(*stampAt(1)));
            EXPECT_EQ(trace.getScene()->placements().getCounts().mPlaced, 0u);
        }

        /// The mask is an inclusion mask, AND-ed at every node — so a category left out of it is
        /// dropped wherever it appears below, which is the shape the weather bug had.
        TEST(RtxOffscreenTraceTest, theSubjectMaskKeepsTheWalkOutOfWhatItDoesNotName)
        {
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

            OffscreenTrace trace(renderer,
                ViewRequest{ .mWidth = 64,
                    .mHeight = 64,
                    .mRayMask = Shaders::MASK_EVERY_CLASS,
                    .mSubject = subject.get(),
                    .mSubjectMask = wanted });
            ASSERT_TRUE(trace.rebuildSubject(*stampAt(1)));

            // One of the two, and the same fixture with `wanted | other` would take both — which is
            // what says the mask is doing the choosing rather than the fixture.
            EXPECT_EQ(trace.getScene()->placements().getCounts().mPlaced, 1u);

            OffscreenTrace both(renderer,
                ViewRequest{ .mWidth = 64,
                    .mHeight = 64,
                    .mRayMask = Shaders::MASK_EVERY_CLASS,
                    .mSubject = subject.get(),
                    .mSubjectMask = wanted | other });
            ASSERT_TRUE(both.rebuildSubject(*stampAt(1)));
            EXPECT_EQ(both.getScene()->placements().getCounts().mPlaced, 2u);
        }
    }
}
