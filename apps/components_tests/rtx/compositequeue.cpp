#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/compositequeue.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/vfs/pathutil.hpp>

namespace Rtx
{
    namespace
    {
        /// A distant chunk of two ground types, which is a chunk that wants flattening.
        Index addChunk(SceneDesc& scene, const VFS::Path::NormalizedView under, const VFS::Path::NormalizedView over)
        {
            constexpr std::array<float, 4> weights{ 1.0f, 0.0f, 0.0f, 1.0f };

            std::array<MaterialLayer, 2> layers{};
            layers[0].mDiffuse = scene.textures().add(under);
            layers[1].mDiffuse = scene.textures().add(over);
            layers[1].mMask = scene.materials().addMask(weights);
            layers[1].mPlacing.mMaskWidth = 2;
            layers[1].mPlacing.mMaskHeight = 2;

            Material material;
            material.mKind = MaterialKind::Terrain;
            material.mFlatten = true;
            material.mLayers = scene.materials().addLayers(layers);

            return scene.addMaterial(material);
        }

        /// One frame of the uploader's sequence around the queue: take, describe what was taken,
        /// then let go of it and of the arrivals, so the next frame's gather sees only what the
        /// next walk writes.
        std::size_t frame(CompositeQueue& queue, SceneDesc& scene)
        {
            const std::size_t taken = queue.advance(scene);
            queue.releaseFinished();
            scene.clearArrivals();
            return taken;
        }

        /// A chunk that asks is given its slot on the frame it asked, and asks once.
        ///
        /// The slot is the composite the device fills in the placement after, so the frame is a
        /// count and never a baker's finishing time: no thread, no sleep and no timing anywhere.
        TEST(RtxCompositeQueueTest, aChunkIsGivenItsSlotOnTheFrameItAskedAndAsksOnce)
        {
            SceneDesc scene;
            const Index chunk = addChunk(
                scene, VFS::Path::NormalizedView("textures/under.dds"), VFS::Path::NormalizedView("textures/over.dds"));

            CompositeQueue queue;
            EXPECT_EQ(queue.advance(scene), 1u) << "the chunk that asked was not given its slot";

            const Index baked = scene.materials().getRows()[chunk].mDiffuse;
            ASSERT_NE(baked, sNoIndex) << "the chunk still shades from its stack";
            EXPECT_EQ(queue.find(baked), chunk) << "the slot the chunk was given is not named as its ground";
            EXPECT_EQ(queue.find(baked + 1), sNoIndex);

            // A slot given out is let go of after the arrival that described it, and a chunk with its
            // ground asks for no more: the rewrite that gave it the slot is a row written, and the
            // gather has to read it as answered rather than as asking again.
            queue.releaseFinished();
            EXPECT_EQ(queue.find(baked), sNoIndex);
            scene.clearArrivals();
            EXPECT_EQ(frame(queue, scene), 0u) << "a chunk with its ground asked again";
        }

        /// A frame takes `sCompositesPerFrame` and no more, in the order the chunks asked.
        ///
        /// **The bound is on what an arrival frame pays**, a texture stood and a dispatch over it
        /// apiece — so a walk that queued a region's worth still takes them a couple at a time, and
        /// the first asked is the first flattened.
        TEST(RtxCompositeQueueTest, aFrameTakesNoMoreThanItsBoundInTheOrderAsked)
        {
            SceneDesc scene;
            constexpr std::size_t chunks = sCompositesPerFrame * 3;

            // Held rather than made per call: a view does not own its path, and each chunk wants two
            // of its own so that no two share a texture slot.
            std::vector<VFS::Path::Normalized> paths;
            std::vector<Index> materials;
            paths.reserve(chunks * 2);
            for (std::size_t at = 0; at < chunks; ++at)
            {
                paths.emplace_back("textures/ground" + std::to_string(at) + "a.dds");
                paths.emplace_back("textures/ground" + std::to_string(at) + "b.dds");
                materials.push_back(addChunk(scene, paths[at * 2], paths[at * 2 + 1]));
            }

            CompositeQueue queue;
            for (std::size_t taken = 0; taken < chunks; taken += sCompositesPerFrame)
            {
                EXPECT_EQ(frame(queue, scene), sCompositesPerFrame)
                    << "a frame took other than its bound with " << (chunks - taken) << " waiting";

                for (std::size_t at = 0; at < chunks; ++at)
                    EXPECT_EQ(scene.materials().getRows()[materials[at]].mDiffuse != sNoIndex,
                        at < taken + sCompositesPerFrame)
                        << "chunk " << at << " after " << (taken + sCompositesPerFrame) << " were taken";
            }

            EXPECT_EQ(frame(queue, scene), 0u) << "more were given out than ever asked";
        }

        /// A chunk whose slot another material took over while it waited is not flattened as
        /// what it asked for: the ask is dropped, and what stands there now asks for itself.
        TEST(RtxCompositeQueueTest, aSlotTakenOverWhileItWaitedIsNotGivenTheFirstAskersGround)
        {
            SceneDesc scene;
            constexpr std::size_t chunks = sCompositesPerFrame + 1;

            std::vector<VFS::Path::Normalized> paths;
            std::vector<Index> materials;
            paths.reserve(chunks * 2 + 3);
            for (std::size_t at = 0; at < chunks; ++at)
            {
                paths.emplace_back("textures/ground" + std::to_string(at) + "a.dds");
                paths.emplace_back("textures/ground" + std::to_string(at) + "b.dds");
                materials.push_back(addChunk(scene, paths[at * 2], paths[at * 2 + 1]));
            }

            CompositeQueue queue;
            EXPECT_EQ(frame(queue, scene), sCompositesPerFrame);

            // The one still waiting goes away, and a wall takes its slot.
            const Index waiting = materials.back();
            materials.pop_back();
            ASSERT_TRUE(scene.release({}, materials)) << "the material was not freed";

            paths.emplace_back("textures/wall.dds");
            Material wall;
            wall.mDiffuse = scene.textures().add(paths.back());
            const Index newcomer = scene.addMaterial(wall);
            ASSERT_EQ(newcomer, waiting) << "the newcomer did not take the freed slot";

            EXPECT_EQ(frame(queue, scene), 0u) << "the chunk that went away was flattened onto the wall";
            EXPECT_EQ(scene.materials().getRows()[newcomer].mDiffuse, wall.mDiffuse);

            // And a chunk that takes the slot after that asks for itself, off the row it wrote.
            ASSERT_TRUE(scene.release({}, materials)) << "the wall was not freed";
            paths.emplace_back("textures/newcomer-a.dds");
            paths.emplace_back("textures/newcomer-b.dds");
            const Index chunk = addChunk(scene, paths[chunks * 2 + 1], paths[chunks * 2 + 2]);
            ASSERT_EQ(chunk, waiting);

            EXPECT_EQ(queue.advance(scene), 1u);
            const Index baked = scene.materials().getRows()[chunk].mDiffuse;
            ASSERT_NE(baked, sNoIndex);
            EXPECT_EQ(queue.find(baked), chunk);
        }
    }
}
