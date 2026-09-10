#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <osg/GL>
#include <osg/Image>
#include <osg/ref_ptr>

#include <components/rtx/compositequeue.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "heldimages.hpp"

namespace Rtx
{
    namespace
    {
        /// One ground texture, in a format the renderer takes.
        ///
        /// **Four texels and `GL_RGBA`.** Three-channel images are refused deliberately, so a layer
        /// left to the image manager's own warning image would flatten to nothing and the schedule
        /// below would have no arrival to time.
        osg::ref_ptr<osg::Image> makeGround()
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName("textures/ground.dds");
            image->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            return image;
        }

        /// A distant chunk of two ground types, which is a chunk that wants flattening.
        Index addChunk(SceneDesc& scene, Testing::HeldImages& images, const VFS::Path::NormalizedView under,
            const VFS::Path::NormalizedView over)
        {
            constexpr std::array<float, 4> weights{ 1.0f, 0.0f, 0.0f, 1.0f };

            images.hold(under, makeGround());
            images.hold(over, makeGround());

            std::array<MaterialLayer, 2> layers{};
            layers[0].mDiffuse = scene.addTexture(under);
            layers[1].mDiffuse = scene.addTexture(over);
            layers[1].mMask = scene.addMask(weights);
            layers[1].mMaskWidth = 2;
            layers[1].mMaskHeight = 2;

            Material material;
            material.mKind = MaterialKind::Terrain;
            material.mFlatten = true;
            material.mLayers = scene.addLayers(layers);

            return scene.addMaterial(material);
        }

        /// Which frame a composite lands on is the schedule's answer and never a baker's.
        ///
        /// **The two halves of that pull against each other.** A frame that collected whatever was
        /// ready would take a composite on a different frame in every run, so a settled frame waits
        /// — and a frame that waited for the stack it had just handed over would spend a whole bake
        /// on the frame path. `sBakeFrames` is what holds them apart, and this says the count of
        /// frames decides rather than how fast the machine flattened the ground.
        ///
        /// **No sleep and no timing anywhere here**, which is what a settled run buys: the frame a
        /// composite is due on waits for it, so the counts below are exact on any machine.
        TEST(RtxCompositeQueueTest, aCompositeLandsOnTheFrameTheScheduleSaysAndNotWhenABakerFinished)
        {
            VFS::Manager vfs;
            Testing::HeldImages images(&vfs, 0);

            SceneDesc scene;
            const Index chunk = addChunk(scene, images, VFS::Path::NormalizedView("textures/under.dds"),
                VFS::Path::NormalizedView("textures/over.dds"));

            CompositeQueue queue;
            queue.setSettled(true);

            // **The frame that hands a stack over never collects it**, which is the whole of what
            // `sBakeFrames` buys. A frame that did would wait out the bake it had just asked for,
            // and the assertion below is what says the slack is there rather than merely spelled.
            EXPECT_EQ(queue.advance(scene, images), 0u) << "the frame that handed the stack over collected it";

            // Nor does any frame before the one it comes due on, however long ago a baker finished.
            for (std::size_t frame = 2; frame <= sBakeFrames; ++frame)
            {
                EXPECT_EQ(queue.advance(scene, images), 0u) << "a composite was collected on frame " << frame;
                EXPECT_EQ(scene.getTables().mMaterials.getRows()[chunk].mDiffuse, sNoIndex)
                    << "the chunk was given ground it is not due yet";
            }

            EXPECT_EQ(queue.advance(scene, images), 1u) << "the composite did not land on the frame it came due";

            const Index baked = scene.getTables().mMaterials.getRows()[chunk].mDiffuse;
            ASSERT_NE(baked, sNoIndex) << "the chunk still shades from its stack";
            EXPECT_NE(queue.find(baked), nullptr) << "the slot the chunk was given holds no composite";

            // And nothing arrives twice: the chunk has its ground and asks for no more.
            queue.releaseFinished();
            EXPECT_EQ(queue.advance(scene, images), 0u);
        }

        /// A frame collects `sCompositesPerFrame` and no more, however many are due.
        ///
        /// **The bound is on what an arrival frame pays**, which is a texture created and staged
        /// apiece — so a walk that queued a region's worth still takes them a couple at a time.
        TEST(RtxCompositeQueueTest, aFrameTakesNoMoreThanItsBoundHoweverManyAreDue)
        {
            VFS::Manager vfs;
            Testing::HeldImages images(&vfs, 0);

            SceneDesc scene;
            constexpr std::size_t chunks = sCompositesPerFrame * 3;

            // Held rather than made per call: a view does not own its path, and each chunk wants two
            // of its own so that no two share a texture slot.
            std::vector<VFS::Path::Normalized> paths;
            paths.reserve(chunks * 2);
            for (std::size_t at = 0; at < chunks; ++at)
            {
                paths.emplace_back("textures/ground" + std::to_string(at) + "a.dds");
                paths.emplace_back("textures/ground" + std::to_string(at) + "b.dds");
                addChunk(scene, images, paths[at * 2], paths[at * 2 + 1]);
            }

            CompositeQueue queue;
            queue.setSettled(true);

            for (std::size_t frame = 1; frame <= sBakeFrames; ++frame)
                EXPECT_EQ(queue.advance(scene, images), 0u) << "a composite was collected on frame " << frame;

            // Every one of them is due at once, because they were all handed over on frame one.
            for (std::size_t taken = 0; taken < chunks; taken += sCompositesPerFrame)
            {
                queue.releaseFinished();
                EXPECT_EQ(queue.advance(scene, images), sCompositesPerFrame)
                    << "a frame took other than its bound with " << (chunks - taken) << " due";
            }

            queue.releaseFinished();
            EXPECT_EQ(queue.advance(scene, images), 0u) << "more arrived than were ever asked for";
        }
    }
}
