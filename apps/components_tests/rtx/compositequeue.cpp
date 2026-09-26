#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/compositequeue.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/vfs/pathutil.hpp>

#include "support/layers.hpp"

namespace Rtx
{
    namespace
    {
        /// A distant chunk of two ground types, which is a chunk that wants flattening. `reflecting`
        /// makes the top one an authored layer, which is a chunk that wants a gloss too.
        Index addChunk(SceneDesc& scene, const VFS::Path::NormalizedView under, const VFS::Path::NormalizedView over,
            const bool reflecting = false)
        {
            constexpr std::array<float, 4> weights{ 1.0f, 0.0f, 0.0f, 1.0f };

            std::array layers{
                Testing::layerOf(scene.textures().add(under)),
                Testing::layerOf(scene.textures().add(over), scene.materials().addMask(weights), 2, 2),
            };
            if (reflecting)
                layers[1].mFlags = Shaders::LAYER_AUTHORED;

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

        /// A chunk that asks is given its slot on the frame it asked, and asks once — and **a chunk
        /// with a layer that reflects is given its gloss beside it**, as its material's specular,
        /// where one with none is given no gloss at all.
        ///
        /// The slot is the composite the device fills in the placement after, so the frame is a
        /// count and never a baker's finishing time: no thread, no sleep and no timing anywhere.
        TEST(RtxCompositeQueueTest, aChunkIsGivenItsSlotOnTheFrameItAskedAndAsksOnce)
        {
            for (const bool reflecting : { false, true })
            {
                SceneDesc scene;
                const Index chunk = addChunk(scene, VFS::Path::NormalizedView("textures/under.dds"),
                    VFS::Path::NormalizedView("textures/over.dds"), reflecting);

                CompositeQueue queue;
                EXPECT_EQ(queue.advance(scene), 1u) << "the chunk that asked was not given its slot";

                const Material& given = scene.materials().getRows()[chunk];
                const Index baked = given.mDiffuse;
                ASSERT_NE(baked, sNoIndex) << "the chunk still shades from its stack";
                EXPECT_EQ(queue.find(baked).mMaterial, chunk)
                    << "the slot the chunk was given is not named as its ground";
                EXPECT_FALSE(queue.find(baked).mGloss);

                if (reflecting)
                {
                    ASSERT_NE(given.mSpecular, sNoIndex) << "a chunk that reflects was given no gloss";
                    EXPECT_NE(given.mSpecular, baked);
                    EXPECT_EQ(queue.find(given.mSpecular).mMaterial, chunk);
                    EXPECT_TRUE(queue.find(given.mSpecular).mGloss) << "the gloss is named as the albedo";
                }
                else
                    EXPECT_EQ(given.mSpecular, sNoIndex) << "a chunk that reflects nowhere was given a gloss";

                EXPECT_EQ(queue.find(static_cast<Index>(scene.textures().getRows().size())).mMaterial, sNoIndex)
                    << "a slot past every slot the table holds";

                // A slot given out is let go of after the arrival that described it, and a chunk with
                // its ground asks for no more: the rewrite that gave it the slot is a row written, and
                // the gather has to read it as answered rather than as asking again.
                queue.releaseFinished();
                EXPECT_EQ(queue.find(baked).mMaterial, sNoIndex);
                scene.clearArrivals();
                EXPECT_EQ(frame(queue, scene), 0u) << "a chunk with its ground asked again";
            }
        }

        /// A frame takes `sCompositesPerFrame` and no more, in the order the chunks asked.
        ///
        /// **The bound is on what an arrival frame pays**, a texture stood and a dispatch over it
        /// apiece — so a walk that queued a region's worth still takes them a couple at a time, and
        /// the first asked is the first flattened.
        ///
        /// **Two waves, so the order survives the ring wrapping and growing.** The first six fill a
        /// ring of eight from the front and a frame takes two, which leaves four from slot two on;
        /// of the second five, four wrap round into slots six, seven, nought and one, and the fifth
        /// finds the ring full and grows it with the oldest ask anywhere but slot nought.
        TEST(RtxCompositeQueueTest, aFrameTakesNoMoreThanItsBoundInTheOrderAsked)
        {
            SceneDesc scene;
            constexpr std::size_t first = sCompositesPerFrame * 3;
            constexpr std::size_t second = sCompositesPerFrame * 2 + 1;

            // Held rather than made per call: a view does not own its path, and each chunk wants two
            // of its own so that no two share a texture slot.
            std::vector<VFS::Path::Normalized> paths;
            std::vector<Index> materials;
            paths.reserve((first + second) * 2);
            const auto ask = [&](const std::size_t count) {
                for (std::size_t at = 0; at < count; ++at)
                {
                    const std::size_t chunk = materials.size();
                    paths.emplace_back("textures/ground" + std::to_string(chunk) + "a.dds");
                    paths.emplace_back("textures/ground" + std::to_string(chunk) + "b.dds");
                    materials.push_back(addChunk(scene, paths[chunk * 2], paths[chunk * 2 + 1]));
                }
            };

            CompositeQueue queue;
            std::size_t taken = 0;
            const auto expectTaken = [&](const std::size_t now) {
                EXPECT_EQ(frame(queue, scene), now) << "a frame took other than its bound";
                taken += now;

                for (std::size_t at = 0; at < materials.size(); ++at)
                    EXPECT_EQ(scene.materials().getRows()[materials[at]].mDiffuse != sNoIndex, at < taken)
                        << "chunk " << at << " after " << taken << " were taken";
            };

            ask(first);
            expectTaken(sCompositesPerFrame);

            ask(second);
            while (taken < materials.size())
                expectTaken(std::min(sCompositesPerFrame, materials.size() - taken));

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
            EXPECT_EQ(queue.find(baked).mMaterial, chunk);
        }
    }
}
