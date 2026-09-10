#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/index.hpp>
#include <components/rtx/meshrange.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/slotset.hpp>
#include <components/rtxvulkan/blockedbuffer.hpp>
#include <components/rtxvulkan/bottomlevelstore.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/frameslots.hpp>
#include <components/rtxvulkan/graveyard.hpp>
#include <components/rtxvulkan/slottable.hpp>
#include <components/rtxvulkan/structurebuild.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// A level grid of `side` quads a side at height `z`: a cell's ground at sixty-four, which
        /// is large enough that a tight copy of its structure is smaller than the loose one.
        Index addGrid(SceneDesc& scene, const unsigned side, const float z)
        {
            std::vector<osg::Vec3f> positions;
            std::vector<osg::Vec3f> normals;
            std::vector<std::uint32_t> indices;
            for (unsigned y = 0; y <= side; ++y)
                for (unsigned x = 0; x <= side; ++x)
                {
                    positions.emplace_back(static_cast<float>(x), static_cast<float>(y), z);
                    normals.emplace_back(0.0f, 0.0f, 1.0f);
                }
            for (unsigned y = 0; y < side; ++y)
                for (unsigned x = 0; x < side; ++x)
                {
                    const std::uint32_t corner = y * (side + 1) + x;
                    indices.insert(indices.end(),
                        { corner, corner + 1, corner + side + 2, corner, corner + side + 2, corner + side + 1 });
                }
            return scene.addMesh(positions, normals, {}, indices);
        }

        struct RtxBottomLevelStoreTest : Testing::DeviceTest
        {
            static constexpr std::uint32_t sSlots = 2;

            SceneDesc mScene;
            SlotBlocks mPoses{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };
            BlockedBuffer mIndices{ Shaders::INDEX_BLOCK, sizeof(std::uint32_t) };

            /// Puts the scene's index runs on the device, which is what a build reads through.
            void stage()
            {
                const Device& device = getDevice();
                const SceneTables tables = mScene.getTables();

                mPoses.open(device, 1, sBuildInputUsage, "poses");
                mIndices.open(device, sBuildInputUsage, "indices");

                Batch setup(getPool());
                mIndices.reserve(setup, static_cast<std::uint32_t>(tables.mMeshes.getIndices().size()));
                for (const MeshRange& range : tables.mMeshes.getRows())
                    mIndices.writeAt(setup, range.mIndices.mOffset, range.mIndices.in(tables.mMeshes.getIndices()));
                orderStagedWrites(setup);
                setup.flush();

                mPoses.settle(FrameSlot{});
            }

            void build(BottomLevelStore& store, std::span<const Index> meshes, Graveyard& graveyard)
            {
                Batch batch(getPool());
                store.build(batch, mScene.getTables(), meshes, mPoses.at(FrameSlot{}), mIndices, graveyard);
                batch.flush();
            }

            /// One placement: what it copies is recorded and run, so the next can build on it.
            const SlotSet& place(BottomLevelStore& store, Graveyard& graveyard)
            {
                const SlotSet& moved = store.prepareCompaction(graveyard);
                if (!moved.empty())
                {
                    Batch batch(getPool());
                    store.recordCompaction(batch.getCommands(), nullptr);
                    batch.flush();
                }
                return moved;
            }
        };

        /// A structure's answer is read when its own placement has run, whatever was built after it.
        ///
        /// **The store asked about every loose structure at every build and read the answers only
        /// once no build had followed for `sSlots` placements**, so a route that builds on every frame
        /// never read one: nothing was copied tight and every build asked the device about the whole
        /// scene over again. Three builds on three placements in a row, and each answer read on the
        /// placement its own question became readable.
        TEST_F(RtxBottomLevelStoreTest, aBuildThatFollowsDoesNotPostponeTheCompactionOfWhatWasBuiltBefore)
        {
            if (mHarness == nullptr)
                GTEST_SKIP() << "no device";

            const std::array<Index, 3> grids{ addGrid(mScene, 64, 0.0f), addGrid(mScene, 64, 1.0f),
                addGrid(mScene, 64, 2.0f) };
            stage();

            Graveyard graveyard(getDevice(), getPool());
            BottomLevelStore store(getDevice(), sSlots);

            // The first is asked on placement count 0, and readable once the count passes 0 + 2.
            build(store, std::span(grids).subspan(0, 1), graveyard);
            EXPECT_TRUE(place(store, graveyard).empty()) << "an answer read before its placement could have run";
            build(store, std::span(grids).subspan(1, 1), graveyard);
            EXPECT_TRUE(place(store, graveyard).empty()) << "an answer read before its placement could have run";
            build(store, std::span(grids).subspan(2, 1), graveyard);

            const SlotSet& atThree = place(store, graveyard);
            EXPECT_TRUE(atThree.has(grids[0])) << "the first structure was not copied tight on placement three";
            EXPECT_FALSE(atThree.has(grids[1])) << "the second was copied before its placement could have run";
            EXPECT_FALSE(atThree.has(grids[2])) << "the third was copied before its placement could have run";

            EXPECT_TRUE(place(store, graveyard).has(grids[1])) << "the second was not copied tight on placement four";
            EXPECT_TRUE(place(store, graveyard).has(grids[2])) << "the third was not copied tight on placement five";

            EXPECT_TRUE(place(store, graveyard).empty()) << "something was copied twice";
            EXPECT_EQ(store.getCompactableBytes(), 0u) << "an answer outlived its copy";
            EXPECT_EQ(store.getCompactableNowBytes(), 0u) << "an answer outlived its copy";

            graveyard.clear();
        }

        /// A structure released before its answer is read is neither read nor copied, and what the
        /// report says is left to save does not count it.
        TEST_F(RtxBottomLevelStoreTest, aStructureReleasedBeforeItsAnswerIsReadIsForgotten)
        {
            if (mHarness == nullptr)
                GTEST_SKIP() << "no device";

            const std::array<Index, 2> grids{ addGrid(mScene, 64, 0.0f), addGrid(mScene, 64, 1.0f) };
            stage();

            Graveyard graveyard(getDevice(), getPool());
            BottomLevelStore store(getDevice(), sSlots);
            build(store, grids, graveyard);

            store.release(std::span(grids).subspan(0, 1), graveyard);
            EXPECT_EQ(store.getStructure(grids[0]), VK_NULL_HANDLE);

            EXPECT_TRUE(place(store, graveyard).empty());
            EXPECT_TRUE(place(store, graveyard).empty());

            const SlotSet& moved = place(store, graveyard);
            EXPECT_FALSE(moved.has(grids[0])) << "a released structure was copied";
            EXPECT_TRUE(moved.has(grids[1])) << "the structure still standing was not copied";

            EXPECT_EQ(store.getCompactableBytes(), 0u);
            EXPECT_EQ(store.getCompactableNowBytes(), 0u);

            graveyard.clear();
        }
    }
}
