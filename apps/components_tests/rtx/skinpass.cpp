#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtxvulkan/barriers.hpp>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/graveyard.hpp>
#include <components/rtxvulkan/skinpass.hpp>
#include <components/rtxvulkan/skintables.hpp>
#include <components/rtxvulkan/slottable.hpp>

#include "geometry.hpp"
#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// A run word: `first << RUN_COUNT_BITS | count`.
        constexpr std::uint32_t run(std::uint32_t first, std::uint32_t count)
        {
            return (first << Shaders::RUN_COUNT_BITS) | count;
        }

        /// A translation `z` up, as the kernel reads it.
        Shaders::GpuBone boneUp(float z)
        {
            return toGpuBone(osg::Matrixf::translate(0.0f, 0.0f, z));
        }

        /// The vector at `vertex` of a block copied back whole.
        osg::Vec3f readVector(const Buffer& staging, std::uint32_t vertex)
        {
            osg::Vec3f value;
            std::memcpy(
                &value, static_cast<const std::byte*>(staging.map()) + vertex * sizeof(osg::Vec3f), sizeof(value));
            return value;
        }

        struct RtxSkinPassTest : Testing::DeviceTest
        {
        };

        /// What the two kernels write, read back and compared against arithmetic done by hand.
        ///
        /// **Five meshes in one scene, because the offsets are half of what is being tested.** A
        /// mesh posed into a neighbour's run would look right on its own and wrong beside it, so
        /// every vertex of every posed run is asserted, and the static quad stands between two of
        /// them: it holds a run among the normals and none at all among the poses. Every expected
        /// value is exact in float: translations, a quarter-and-three-quarters blend of two of
        /// them, a rotation of nought-and-one entries, and a half of a unit offset.
        TEST_F(RtxSkinPassTest, theKernelsPoseEachMeshIntoItsOwnRunAndLeaveTheRestAlone)
        {
            Device& device = getDevice();
            CommandPool& pool = getPool();

            SceneDesc scene;

            // One bone over the whole quad, every weight one.
            const std::array oneRuns{ run(0, 1), run(0, 1), run(0, 1), run(0, 1) };
            const std::array oneInfluence{ Shaders::GpuInfluence{ .mBone = 0, .mWeight = 1.0f } };
            const Index oneBone = scene.deformers().addRig(oneRuns, oneInfluence, 1);

            // Two bones, and the third vertex a blend of them: a quarter of the first and three
            // quarters of the second.
            const std::array twoRuns{ run(0, 1), run(0, 1), run(1, 2), run(0, 1) };
            const std::array twoInfluences{
                Shaders::GpuInfluence{ .mBone = 0, .mWeight = 1.0f },
                Shaders::GpuInfluence{ .mBone = 0, .mWeight = 0.25f },
                Shaders::GpuInfluence{ .mBone = 1, .mWeight = 0.75f },
            };
            const Index twoBones = scene.deformers().addRig(twoRuns, twoInfluences, 2);

            // Two targets over the quad: the base's zeroes and a unit lift.
            std::array<osg::Vec3f, 8> offsets{};
            for (std::size_t at = 4; at < 8; ++at)
                offsets[at] = osg::Vec3f(0.0f, 0.0f, 1.0f);
            const Index lift = scene.deformers().addMorph(offsets, 2);

            const std::array sideways{
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
            };
            const std::array upward{
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
            };

            const Index raised = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = upward, .mIndices = Testing::sQuadIndices },
                {}, Deform::Rig, oneBone);
            const Index still = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = upward, .mIndices = Testing::sQuadIndices });
            const Index blended = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = upward, .mIndices = Testing::sQuadIndices },
                {}, Deform::Rig, twoBones);
            const Index turned = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = sideways, .mIndices = Testing::sQuadIndices },
                {}, Deform::Rig, oneBone);
            const Index lifted = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = upward, .mIndices = Testing::sQuadIndices },
                {}, Deform::Morph, lift);

            const osg::BoundingBoxf anywhere(osg::Vec3f(), osg::Vec3f(1.0f, 1.0f, 1.0f));

            const std::array atFive{ boneUp(5.0f) };
            scene.poseRig(raised, atFive, anywhere);

            const std::array fourAndEight{ boneUp(4.0f), boneUp(8.0f) };
            scene.poseRig(blended, fourAndEight, anywhere);

            // A quarter turn about z, in OpenSceneGraph's row-vector convention: `(x, y)` goes to
            // `(-y, x)`, and so does a normal along x.
            const std::array quarterTurn{ toGpuBone(osg::Matrixf(
                0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f)) };
            scene.poseRig(turned, quarterTurn, anywhere);

            const std::array halfway{ 1.0f, 0.5f };
            scene.poseMorph(lifted, halfway, anywhere);

            ASSERT_EQ(scene.meshes().getDeformed().size(), 4u);

            // The pass's destination, owned here so it can be copied back: the renderer's own blocks
            // are build input and never a transfer source.
            //
            // **Two lengths, because the pass writes two spaces.** A hit reads a normal, so every
            // mesh has a run among them; nothing reads a position at a hit, so the poses hold the
            // four deforming quads and not the static one between them.
            const auto vertices = static_cast<std::uint32_t>(scene.meshes().getPositions().size());
            const std::uint32_t posedVertices = scene.deformers().getBindVertexCount();
            EXPECT_EQ(vertices, 20u) << "five quads of four vertices";
            EXPECT_EQ(posedVertices, 16u) << "the static quad took a run in the pose table";

            constexpr VkBufferUsageFlags readable
                = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            SlotBlocks poses{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };
            SlotBlocks normals{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };
            poses.open(device, 2, readable, "posed positions");
            normals.open(device, 2, readable, "posed normals");
            // Its own scope, because the blocks are device memory: the fills have to reach the
            // queue before the dispatch below reads what they left.
            {
                Batch setup(pool);
                poses.reserve(setup, posedVertices);
                normals.reserve(setup, vertices);
                setup.flush();
            }
            for (std::uint32_t slot = 0; slot < 2; ++slot)
            {
                poses.settle(FrameSlot{ slot });
                normals.settle(FrameSlot{ slot });
            }

            Graveyard graveyard(device, pool);
            Batch tableSetup(pool);
            SkinTables tables(device, tableSetup, scene, 2, graveyard);
            tableSetup.flush();
            const SkinPass pass(device, Testing::getShaderDirectory());

            const VkDeviceSize poseBytes = VkDeviceSize{ posedVertices } * sizeof(osg::Vec3f);
            const VkDeviceSize normalBytes = VkDeviceSize{ vertices } * sizeof(osg::Vec3f);
            const Buffer readPositions = Buffer::staging(device, poseBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "test");
            const Buffer readNormals = Buffer::staging(device, normalBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "test");

            /// Poses what `slot` owes and copies its whole first block back.
            const auto poseAndRead = [&](FrameSlot slot) {
                bool recorded = false;
                pool.submitAndWait([&](VkCommandBuffer commands) {
                    recorded = pass.record(commands, scene, slot, tables, poses, normals, nullptr);

                    handOver(commands, Use::sBufferComputeWrite,
                        BufferUse{ VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT });

                    poses.at(slot).getBlock(0).copyTo(commands, readPositions, poseBytes);
                    normals.at(slot).getBlock(0).copyTo(commands, readNormals, normalBytes);
                });

                return recorded;
            };

            EXPECT_TRUE(poseAndRead(FrameSlot{ 0 })) << "four meshes owed and nothing recorded";

            const auto positionOf = [&](Index mesh, std::uint32_t vertex) {
                return readVector(readPositions, scene.meshes().getRows()[mesh].mBindOffset + vertex);
            };
            const auto normalOf = [&](Index mesh, std::uint32_t vertex) {
                return readVector(readNormals, scene.meshes().getRows()[mesh].mVertices.mOffset + vertex);
            };

            // One bone at five: every corner five up, and an upward normal left as it was.
            for (std::uint32_t vertex = 0; vertex < 4; ++vertex)
            {
                EXPECT_EQ(positionOf(raised, vertex), Testing::sUnitQuad[vertex] + osg::Vec3f(0.0f, 0.0f, 5.0f))
                    << vertex;
                EXPECT_EQ(normalOf(raised, vertex), osg::Vec3f(0.0f, 0.0f, 1.0f)) << vertex;
            }

            // The blend: 0.25 · 4 + 0.75 · 8 = 1 + 6 = 7 on the third corner, and four on the rest.
            EXPECT_EQ(positionOf(blended, 2), osg::Vec3f(1.0f, 1.0f, 7.0f));
            EXPECT_EQ(positionOf(blended, 0), osg::Vec3f(0.0f, 0.0f, 4.0f));
            EXPECT_EQ(positionOf(blended, 1), osg::Vec3f(1.0f, 0.0f, 4.0f));
            EXPECT_EQ(positionOf(blended, 3), osg::Vec3f(0.0f, 1.0f, 4.0f));
            EXPECT_EQ(normalOf(blended, 2), osg::Vec3f(0.0f, 0.0f, 1.0f))
                << "a blend of two translations turns nothing";

            // The quarter turn: `(1, 0)` to `(0, 1)`, `(1, 1)` to `(-1, 1)`, and the normal along x
            // to along y — the linear part alone, and no inverse transpose.
            EXPECT_EQ(positionOf(turned, 0), osg::Vec3f(0.0f, 0.0f, 0.0f));
            EXPECT_EQ(positionOf(turned, 1), osg::Vec3f(0.0f, 1.0f, 0.0f));
            EXPECT_EQ(positionOf(turned, 2), osg::Vec3f(-1.0f, 1.0f, 0.0f));
            EXPECT_EQ(positionOf(turned, 3), osg::Vec3f(-1.0f, 0.0f, 0.0f));
            EXPECT_EQ(normalOf(turned, 0), osg::Vec3f(0.0f, 1.0f, 0.0f));

            // The morph: half of a unit lift on every corner, and a normal a morph never touches
            // still holding whatever the block held — nothing, because the pass wrote no normal.
            for (std::uint32_t vertex = 0; vertex < 4; ++vertex)
                EXPECT_EQ(positionOf(lifted, vertex), Testing::sUnitQuad[vertex] + osg::Vec3f(0.0f, 0.0f, 0.5f))
                    << vertex;
            EXPECT_EQ(normalOf(lifted, 0), osg::Vec3f()) << "a morph moved a normal";

            // **And the quad between them is untouched among the normals.** Its run holds what the
            // block was made with, which is nothing: a kernel that wrote past its mesh would have
            // landed here. It has no run among the poses at all, which the bind count above says,
            // and the sixteen that are there are each asserted exactly.
            for (std::uint32_t vertex = 0; vertex < 4; ++vertex)
                EXPECT_EQ(normalOf(still, vertex), osg::Vec3f()) << "a pose landed in a static neighbour at " << vertex;

            // **The account: what one copy was paid the other still owes.** A frame that poses
            // nothing new still has to bring the second copy level, and a copy that is level
            // records nothing.
            scene.clearPlacement();
            EXPECT_TRUE(poseAndRead(FrameSlot{ 1 })) << "the second copy owed four poses and nothing was recorded";
            EXPECT_EQ(positionOf(raised, 2), osg::Vec3f(1.0f, 1.0f, 5.0f)) << "the pose reached the second copy";
            EXPECT_EQ(positionOf(blended, 2), osg::Vec3f(1.0f, 1.0f, 7.0f));

            EXPECT_FALSE(poseAndRead(FrameSlot{ 0 })) << "a copy that owed nothing recorded a dispatch";

            // **A run handed out again reaches the copy through the batch.** `blended` goes and a
            // mesh on a new two-bone rig takes every run it held — the slot,
            // the bind run, the rows and the rig's runs and influences, each asserted, because the
            // reuse is what is being tested. Its bind is the quad shifted along x, its rig blends the
            // third corner half and half, and its bones stand at one and three: 0.5 · 1 + 0.5 · 3 =
            // 2 there and 1 elsewhere, so a stale bind, a stale row or a stale influence would each
            // show as a different number.
            const MeshRange went = scene.meshes().getRows()[blended];
            const Rig wentRig = scene.deformers().getRigs()[twoBones];
            scene.clearArrivals();
            const std::array kept{ raised, still, turned, lifted };
            ASSERT_TRUE(scene.release(kept, {}));

            const std::array halfAndHalf{
                Shaders::GpuInfluence{ .mBone = 0, .mWeight = 1.0f },
                Shaders::GpuInfluence{ .mBone = 0, .mWeight = 0.5f },
                Shaders::GpuInfluence{ .mBone = 1, .mWeight = 0.5f },
            };
            const Index twoMore = scene.deformers().addRig(twoRuns, halfAndHalf, 2);
            std::array<osg::Vec3f, 4> shifted = Testing::sUnitQuad;
            for (osg::Vec3f& corner : shifted)
                corner += osg::Vec3f(1.0f, 0.0f, 0.0f);
            const Index arrived = scene.addMesh(
                MeshArrays{ .mPositions = shifted, .mNormals = upward, .mIndices = Testing::sQuadIndices }, {},
                Deform::Rig, twoMore);
            const MeshRange& taken = scene.meshes().getRows()[arrived];
            ASSERT_EQ(arrived, blended) << "the slot was not handed out again";
            ASSERT_EQ(taken.mBindOffset, went.mBindOffset) << "the bind run was not handed out again";
            ASSERT_EQ(taken.mPoseOffset, went.mPoseOffset) << "the rows were not handed out again";
            ASSERT_EQ(scene.deformers().getRigs()[twoMore].mRuns, wentRig.mRuns) << "the run words were not";
            ASSERT_EQ(scene.deformers().getRigs()[twoMore].mInfluences, wentRig.mInfluences)
                << "the influences were not";

            const std::array oneAndThree{ boneUp(1.0f), boneUp(3.0f) };
            scene.poseRig(arrived, oneAndThree, anywhere);

            {
                Batch arrival(pool);
                tables.extend(arrival, scene, graveyard);
                EXPECT_TRUE(pass.recordArrived(
                    arrival.getCommands(), scene, FrameSlot{ 0 }, scene.meshes().getArrived(), tables, poses, normals))
                    << "an arrival with nothing to pose";

                handOver(arrival.getCommands(), Use::sBufferComputeWrite,
                    BufferUse{ VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT });
                poses.at(FrameSlot{ 0 }).getBlock(0).copyTo(arrival.getCommands(), readPositions, poseBytes);
                arrival.flush();
            }

            EXPECT_EQ(positionOf(arrived, 2), osg::Vec3f(2.0f, 1.0f, 2.0f));
            EXPECT_EQ(positionOf(arrived, 0), osg::Vec3f(1.0f, 0.0f, 1.0f));
            EXPECT_EQ(positionOf(arrived, 1), osg::Vec3f(2.0f, 0.0f, 1.0f));
            EXPECT_EQ(positionOf(arrived, 3), osg::Vec3f(1.0f, 1.0f, 1.0f));
            EXPECT_EQ(positionOf(raised, 2), osg::Vec3f(1.0f, 1.0f, 5.0f)) << "an arrival touched a neighbour";
        }
    }
}
