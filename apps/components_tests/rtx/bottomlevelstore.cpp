#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/mesh.hpp>
#include <components/rtx/refusal.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/slots.hpp>
#include <components/rtxvulkan/blockedbuffer.hpp>
#include <components/rtxvulkan/bottomlevelstore.hpp>
#include <components/rtxvulkan/bufferusage.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/frameslots.hpp>
#include <components/rtxvulkan/graveyard.hpp>
#include <components/rtxvulkan/slottable.hpp>
#include <components/rtxvulkan/structurebuild.hpp>

#include "support/device/harness.hpp"
#include "support/device/memorylimits.hpp"

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
            return scene.addMesh(MeshArrays{ .mPositions = positions, .mNormals = normals, .mIndices = indices });
        }

        struct RtxBottomLevelStoreTest : Testing::DeviceTest
        {
            SceneDesc mScene;
            SlotBlocks mPoses{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };
            BlockedBuffer mIndices{ Shaders::INDEX_BLOCK, sizeof(std::uint32_t) };

            /// Puts the scene's index runs on the device, which is what a build reads through.
            void stage()
            {
                const Device& device = getDevice();
                const SceneDesc& tables = mScene;

                mPoses.open(device, 1, sBuildInputUsage, "poses");
                mIndices.open(device, sBuildInputUsage, "indices");

                Batch setup(getPool());
                mIndices.reserve(setup, static_cast<std::uint32_t>(tables.meshes().getIndices().size()));
                for (const MeshRange& range : tables.meshes().getRows())
                    mIndices.writeAt(setup, range.mIndices.mOffset, range.mIndices.in(tables.meshes().getIndices()));
                orderStagedWrites(setup);
                setup.flush();

                mPoses.settle(FrameSlot{});
            }

            /// Builds and waits, so the answers are on the device when this returns.
            void build(BottomLevelStore& store, std::span<const Index> meshes)
            {
                Batch batch(getPool());
                std::vector<Refusal> refused;
                store.build(batch, mScene, meshes, mPoses.at(FrameSlot{}), mIndices, 0, refused);
                EXPECT_TRUE(refused.empty()) << "a device with room refused a structure";
                batch.flush();
            }

            /// Builds into a batch that rides the pool's next submit, which nothing has made yet.
            void buildDeferred(BottomLevelStore& store, std::span<const Index> meshes)
            {
                Batch batch(getPool());
                std::vector<Refusal> refused;
                store.build(batch, mScene, meshes, mPoses.at(FrameSlot{}), mIndices, 0, refused);
                EXPECT_TRUE(refused.empty()) << "a device with room refused a structure";
                batch.defer();
            }

            /// One placement: what it copies is recorded and run, so the next can build on it.
            const SlotSet& place(BottomLevelStore& store)
            {
                const SlotSet& moved = store.prepareCompaction();
                if (!moved.empty())
                {
                    Batch batch(getPool());
                    store.recordCompaction(batch.getCommands(), nullptr);
                    batch.flush();
                }
                return moved;
            }
        };

        /// A structure's answer is read once the submit that carried its question has run, and
        /// not before, whatever was built after it.
        ///
        /// **The store counted placements and called the count a fence**: an answer was read once
        /// no build had followed for two placements, which a route that builds on every frame never
        /// reached, and a crossing that placed twice in one frame ran the count ahead of the queue.
        /// It asks the timeline now. A build that waited is readable on the next placement however
        /// many builds followed; one deferred into a submit nothing has made is not readable until
        /// that submit has run.
        TEST_F(RtxBottomLevelStoreTest, anAnswerIsReadOnceItsOwnSubmitHasRunAndNotBefore)
        {
            const std::array<Index, 3> grids{ addGrid(mScene, 64, 0.0f), addGrid(mScene, 64, 1.0f),
                addGrid(mScene, 64, 2.0f) };
            stage();

            BottomLevelStore store(getDevice());

            // Waited for, so the first placement reads it — and the build after it changes nothing.
            build(store, std::span(grids).subspan(0, 1));
            build(store, std::span(grids).subspan(1, 1));
            const SlotSet& atOne = place(store);
            EXPECT_TRUE(atOne.has(grids[0])) << "a question whose submit has run was not read";
            EXPECT_TRUE(atOne.has(grids[1])) << "a question whose submit has run was not read";

            // Deferred and never submitted: the timeline has not passed the value it rides, so the
            // placement reads nothing, and it is not the count of placements that decides.
            buildDeferred(store, std::span(grids).subspan(2, 1));
            EXPECT_TRUE(place(store).empty()) << "an answer read before its submit could have run";
            EXPECT_TRUE(place(store).empty()) << "an answer read before its submit could have run";

            // Submitted and waited, and the next placement reads it.
            getPool().finishDeferred();
            EXPECT_TRUE(place(store).has(grids[2])) << "an answer whose submit has run was not read";

            EXPECT_TRUE(place(store).empty()) << "something was copied twice";
            EXPECT_EQ(store.getCompactableBytes(), 0u) << "an answer outlived its copy";
            EXPECT_EQ(store.getCompactableNowBytes(), 0u) << "an answer outlived its copy";

            // Before the store goes: a buried structure gives its room back to the store's storage.
            getDevice().waitIdle();
            getDevice().collectIdle();
        }

        /// A structure released before its answer is read is neither read nor copied, and what the
        /// report says is left to save does not count it.
        TEST_F(RtxBottomLevelStoreTest, aStructureReleasedBeforeItsAnswerIsReadIsForgotten)
        {
            const std::array<Index, 2> grids{ addGrid(mScene, 64, 0.0f), addGrid(mScene, 64, 1.0f) };
            stage();

            BottomLevelStore store(getDevice());
            build(store, grids);

            store.release(std::span(grids).subspan(0, 1));
            EXPECT_EQ(store.getStructure(grids[0]), VK_NULL_HANDLE);

            // **A release of a slot this never built, or built and already released, is nothing**
            // — what the hand-over relies on: a slot the scene took and gave back inside one frame
            // reaches the backend as gone, and the last word of a slot given back and taken over
            // is arrived, so the same slot can be told gone again a frame later.
            const std::array<Index, 2> unbuilt{ grids[0], static_cast<Index>(mScene.meshes().size() + 7) };
            store.release(unbuilt);
            EXPECT_EQ(store.getStructure(grids[0]), VK_NULL_HANDLE);
            EXPECT_NE(store.getStructure(grids[1]), VK_NULL_HANDLE);

            const SlotSet& moved = place(store);
            EXPECT_FALSE(moved.has(grids[0])) << "a released structure was copied";
            EXPECT_TRUE(moved.has(grids[1])) << "the structure still standing was not copied";

            EXPECT_EQ(store.getCompactableBytes(), 0u);
            EXPECT_EQ(store.getCompactableNowBytes(), 0u);

            // Before the store goes: a buried structure gives its room back to the store's storage.
            getDevice().waitIdle();
            getDevice().collectIdle();
        }

        /// A mesh the device has no room for is left out: its slot holds no structure, so a row
        /// placing it names none and the top level skips it, and the meshes built before it stand.
        ///
        /// **Room for the frame's memory and none for content's**, which is what the build asks
        /// both of: its scratch and the staged positions are the frame's and are made, and the
        /// structure's room is content's and is refused. The first grid's block was made to its
        /// size, so the second has nowhere to go but a block of its own.
        TEST_F(RtxBottomLevelStoreTest, aMeshTheDeviceHasNoRoomForIsLeftOutAndPlacesNothing)
        {
            const std::array<Index, 2> grids{ addGrid(mScene, 64, 0.0f), addGrid(mScene, 64, 1.0f) };
            stage();

            BottomLevelStore store(getDevice());
            build(store, std::span(grids).subspan(0, 1));

            std::vector<Refusal> refused;
            {
                const Testing::NoRoomForContent full(getDevice());
                Batch batch(getPool());
                store.build(
                    batch, mScene, std::span(grids).subspan(1, 1), mPoses.at(FrameSlot{}), mIndices, 0, refused);
                batch.flush();
            }

            ASSERT_EQ(refused.size(), 1u);
            EXPECT_EQ(refused[0].mKind, Refused::Mesh);
            EXPECT_EQ(refused[0].mWhy, "no device memory is left for it");

            EXPECT_FALSE(store.stands(grids[1]));
            EXPECT_EQ(store.getStructure(grids[1]), VK_NULL_HANDLE);
            EXPECT_EQ(store.getAddress(grids[1]), 0u) << "a row placing a mesh left out would name a structure";
            EXPECT_TRUE(store.stands(grids[0])) << "a mesh built before the device ran out was taken down";
            EXPECT_NE(store.getAddress(grids[0]), 0u);

            build(store, std::span(grids).subspan(1, 1));
            EXPECT_TRUE(store.stands(grids[1]));

            // Before the store goes: a buried structure gives its room back to the store's storage.
            getDevice().waitIdle();
            getDevice().collectIdle();
        }
    }
}
