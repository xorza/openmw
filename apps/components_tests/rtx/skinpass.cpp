#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include <osg/BoundingBox>
#include <osg/Matrixf>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/deformertable.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/skinning.h>
#include <components/rtx/shaders/tangent.h>
#include <components/rtx/tangent.hpp>
#include <components/rtxvulkan/barriers.hpp>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/frameslots.hpp>
#include <components/rtxvulkan/imageuse.hpp>
#include <components/rtxvulkan/skinpass.hpp>
#include <components/rtxvulkan/skintables.hpp>
#include <components/rtxvulkan/slottable.hpp>

#include "support/device/harness.hpp"
#include "support/device/heldsubmit.hpp"
#include "support/geometry.hpp"

namespace Rtx
{
    namespace
    {
        /// The element at `vertex` of a block copied back whole.
        template <class T>
        T readAt(const Buffer& copied, std::uint32_t vertex)
        {
            T value;
            std::memcpy(&value, static_cast<const std::byte*>(copied.map()) + vertex * sizeof(T), sizeof(T));
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
        /// them, a rotation of nought-and-one entries, and a half of a unit offset. The tangents
        /// are exact as well, because a quarter turn moves a packed tangent from one step of the
        /// octahedron to another, and the host's packing of it is the device's to the bit.
        TEST_F(RtxSkinPassTest, theKernelsPoseEachMeshIntoItsOwnRunAndLeaveTheRestAlone)
        {
            Device& device = getDevice();
            CommandPool& pool = getPool();

            SceneDesc scene;

            // One bone over the whole quad, every weight one.
            const std::array oneRuns{ Shaders::runWord(0, 1), Shaders::runWord(0, 1), Shaders::runWord(0, 1),
                Shaders::runWord(0, 1) };
            const std::array oneInfluence{ Shaders::GpuInfluence{ .mBone = 0, .mWeight = 1.0f } };
            const RigSpec oneBone{ .mRuns = oneRuns, .mInfluences = oneInfluence, .mBones = 1 };

            // Two bones, and the third vertex a blend of them: a quarter of the first and three
            // quarters of the second.
            const std::array twoRuns{ Shaders::runWord(0, 1), Shaders::runWord(0, 1), Shaders::runWord(1, 2),
                Shaders::runWord(0, 1) };
            const std::array twoInfluences{
                Shaders::GpuInfluence{ .mBone = 0, .mWeight = 1.0f },
                Shaders::GpuInfluence{ .mBone = 0, .mWeight = 0.25f },
                Shaders::GpuInfluence{ .mBone = 1, .mWeight = 0.75f },
            };
            const RigSpec twoBones{ .mRuns = twoRuns, .mInfluences = twoInfluences, .mBones = 2 };

            // Two targets over the quad: the base's zeroes and a unit lift.
            std::array<osg::Vec3f, 8> offsets{};
            for (std::size_t at = 4; at < 8; ++at)
                offsets[at] = osg::Vec3f(0.0f, 0.0f, 1.0f);
            const MorphSpec lift{ .mOffsets = offsets, .mTargets = 2 };

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

            // Along x either way round, one off every axis and below the octahedron's equator, and
            // none: what the generator leaves where a triangle's texture coordinates are degenerate.
            const std::array alongX{
                osg::Vec4f(1.0f, 0.0f, 0.0f, 1.0f),
                osg::Vec4f(1.0f, 0.0f, 0.0f, -1.0f),
                osg::Vec4f(1.0f, 2.0f, -4.0f, -1.0f),
                osg::Vec4f(),
            };

            // Each rig with the first mesh on it, and the second mesh on the one-bone rig by its
            // index: the slots come out as they would have with the rigs made first.
            const DeformedMesh onOneBone = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = upward, .mIndices = Testing::sQuadIndices },
                {}, oneBone);
            const Index raised = onOneBone.mMesh;
            const Index still = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = upward, .mIndices = Testing::sQuadIndices });
            const DeformedMesh onTwoBones = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = upward, .mIndices = Testing::sQuadIndices },
                {}, twoBones);
            const Index blended = onTwoBones.mMesh;
            const Index turned = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad,
                                                   .mNormals = sideways,
                                                   .mTangents = alongX,
                                                   .mIndices = Testing::sQuadIndices },
                {}, onOneBone.mDeformer);
            const Index lifted = scene
                                     .addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad,
                                                  .mNormals = upward,
                                                  .mIndices = Testing::sQuadIndices },
                                         {}, lift)
                                     .mMesh;

            const osg::BoundingBoxf anywhere(osg::Vec3f(), osg::Vec3f(1.0f, 1.0f, 1.0f));

            const std::array atFive{ Testing::boneUp(5.0f) };
            Testing::poseRig(scene, raised, atFive, anywhere);

            const std::array fourAndEight{ Testing::boneUp(4.0f), Testing::boneUp(8.0f) };
            Testing::poseRig(scene, blended, fourAndEight, anywhere);

            // A quarter turn about z, in OpenSceneGraph's row-vector convention: `(x, y)` goes to
            // `(-y, x)`, and so does a normal along x.
            const std::array quarterTurn{ toGpuBone(osg::Matrixf(
                0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f)) };
            Testing::poseRig(scene, turned, quarterTurn, anywhere);

            const std::array halfway{ 1.0f, 0.5f };
            Testing::poseMorph(scene, lifted, halfway, anywhere);

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
            SlotBlocks tangents{ Shaders::VERTEX_BLOCK, sizeof(std::uint32_t) };
            poses.open(device, 2, readable, "posed positions");
            normals.open(device, 2, readable, "posed normals");
            tangents.open(device, 2, readable, "posed tangents");
            // Its own scope, because the blocks are device memory: the fills have to reach the
            // queue before the dispatch below reads what they left.
            {
                Batch setup(pool);
                poses.reserve(setup, posedVertices);
                normals.reserve(setup, vertices);
                tangents.reserve(setup, vertices);
                setup.flush();
            }
            for (std::uint32_t slot = 0; slot < 2; ++slot)
            {
                poses.settle(FrameSlot{ slot });
                normals.settle(FrameSlot{ slot });
                tangents.settle(FrameSlot{ slot });
            }

            Batch tableSetup(pool);
            SkinTables tables(device, tableSetup, scene, 2);
            tableSetup.flush();
            const SkinPass pass(device, Testing::getShaderDirectory());

            const VkDeviceSize poseBytes = VkDeviceSize{ posedVertices } * sizeof(osg::Vec3f);
            const VkDeviceSize normalBytes = VkDeviceSize{ vertices } * sizeof(osg::Vec3f);
            const Buffer readPositions = Buffer::readBack(device, poseBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "test");
            const Buffer readNormals = Buffer::readBack(device, normalBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "test");
            const VkDeviceSize tangentBytes = VkDeviceSize{ vertices } * sizeof(std::uint32_t);
            const Buffer readTangents
                = Buffer::readBack(device, tangentBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "test");

            /// Poses what `slot` owes and copies its whole first block back.
            const auto poseAndRead = [&](FrameSlot slot) {
                bool recorded = false;
                pool.submitAndWait([&](VkCommandBuffer commands) {
                    recorded = pass.record(commands,
                        Skinning{ .mScene = scene,
                            .mSlot = slot,
                            .mTables = tables,
                            .mPoses = poses,
                            .mNormals = normals,
                            .mTangents = tangents });

                    handOver(commands, Use::sBufferComputeWrite,
                        BufferUse{ VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT });

                    poses.at(slot).getBlock(0).copyTo(commands, readPositions, poseBytes);
                    normals.at(slot).getBlock(0).copyTo(commands, readNormals, normalBytes);
                    tangents.at(slot).getBlock(0).copyTo(commands, readTangents, tangentBytes);
                });

                return recorded;
            };

            EXPECT_TRUE(poseAndRead(FrameSlot{ 0 })) << "four meshes owed and nothing recorded";

            const auto positionOf = [&](Index mesh, std::uint32_t vertex) {
                return readAt<osg::Vec3f>(readPositions, scene.meshes().getRows()[mesh].mBindOffset + vertex);
            };
            const auto normalOf = [&](Index mesh, std::uint32_t vertex) {
                return readAt<osg::Vec3f>(readNormals, scene.meshes().getRows()[mesh].mVertices.mOffset + vertex);
            };
            const auto tangentOf = [&](Index mesh, std::uint32_t vertex) {
                return readAt<std::uint32_t>(readTangents, scene.meshes().getRows()[mesh].mVertices.mOffset + vertex);
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

            // The tangent along x to along y as well, its handedness kept. Along y is the square's
            // `(0, 1)`: steps `0 + 16383 = 0x3FFF` and `16383 + 16383 = 32766 = 0x7FFE`, the second
            // fifteen bits up, which is `0x3FFF0000`, and the present bit over them.
            EXPECT_EQ(tangentOf(turned, 0), 0xBFFF3FFFu);
            EXPECT_EQ(tangentOf(turned, 1), 0xFFFF3FFFu) << "the handedness was lost";

            // Off the axes, below the equator: the device's word is the host's for the same turn of
            // the same stored tangent, `(x, y, z)` to `(-y, x, z)`.
            const osg::Vec4f stored = Shaders::unpackTangent(packTangent(alongX[2]));
            EXPECT_EQ(tangentOf(turned, 2), packTangent(osg::Vec4f(-stored.y(), stored.x(), stored.z(), stored.w())));
            EXPECT_EQ(tangentOf(turned, 3), 0u) << "no tangent posed into one";

            // The morph: half of a unit lift on every corner, and a normal a morph never touches
            // still holding whatever the block held — nothing, because the pass wrote no normal.
            for (std::uint32_t vertex = 0; vertex < 4; ++vertex)
                EXPECT_EQ(positionOf(lifted, vertex), Testing::sUnitQuad[vertex] + osg::Vec3f(0.0f, 0.0f, 0.5f))
                    << vertex;
            EXPECT_EQ(normalOf(lifted, 0), osg::Vec3f()) << "a morph moved a normal";
            EXPECT_EQ(tangentOf(lifted, 0), 0u) << "a morph moved a tangent";

            // **And the quad between them is untouched among the normals.** Its run holds what the
            // block was made with, which is nothing: a kernel that wrote past its mesh would have
            // landed here. It has no run among the poses at all, which the bind count above says,
            // and the sixteen that are there are each asserted exactly.
            for (std::uint32_t vertex = 0; vertex < 4; ++vertex)
            {
                EXPECT_EQ(normalOf(still, vertex), osg::Vec3f()) << "a pose landed in a static neighbour at " << vertex;
                EXPECT_EQ(tangentOf(still, vertex), 0u) << "a pose landed in a static neighbour at " << vertex;
                EXPECT_EQ(tangentOf(raised, vertex), 0u) << "a mesh with no tangents was posed some at " << vertex;
            }

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
            const Deformer wentRig = scene.deformers().getDeformers()[onTwoBones.mDeformer];
            scene.clearArrivals();
            const std::array kept{ raised, still, turned, lifted };
            ASSERT_TRUE(scene.release(kept, {}));

            const std::array halfAndHalf{
                Shaders::GpuInfluence{ .mBone = 0, .mWeight = 1.0f },
                Shaders::GpuInfluence{ .mBone = 0, .mWeight = 0.5f },
                Shaders::GpuInfluence{ .mBone = 1, .mWeight = 0.5f },
            };
            std::array<osg::Vec3f, 4> shifted = Testing::sUnitQuad;
            for (osg::Vec3f& corner : shifted)
                corner += osg::Vec3f(1.0f, 0.0f, 0.0f);
            const DeformedMesh moreArrived = scene.addMesh(
                MeshArrays{ .mPositions = shifted, .mNormals = upward, .mIndices = Testing::sQuadIndices }, {},
                RigSpec{ .mRuns = twoRuns, .mInfluences = halfAndHalf, .mBones = 2 });
            const Index twoMore = moreArrived.mDeformer;
            const Index arrived = moreArrived.mMesh;
            const MeshRange& taken = scene.meshes().getRows()[arrived];
            ASSERT_EQ(arrived, blended) << "the slot was not handed out again";
            ASSERT_EQ(taken.mBindOffset, went.mBindOffset) << "the bind run was not handed out again";
            ASSERT_EQ(taken.mPoseOffset, went.mPoseOffset) << "the rows were not handed out again";
            ASSERT_EQ(scene.deformers().getDeformers()[twoMore].mRuns, wentRig.mRuns) << "the run words were not";
            ASSERT_EQ(scene.deformers().getDeformers()[twoMore].mInfluences, wentRig.mInfluences)
                << "the influences were not";

            const std::array oneAndThree{ Testing::boneUp(1.0f), Testing::boneUp(3.0f) };
            Testing::poseRig(scene, arrived, oneAndThree, anywhere);

            {
                Batch arrival(pool);
                tables.extend(arrival, scene);
                EXPECT_TRUE(pass.recordArrived(arrival.getCommands(),
                    Skinning{ .mScene = scene,
                        .mSlot = FrameSlot{ 0 },
                        .mTables = tables,
                        .mPoses = poses,
                        .mNormals = normals,
                        .mTangents = tangents },
                    scene.meshes().getArrived()))
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

        /// A placement into the first copy waits for the arrival that posed it there, and for
        /// nothing longer.
        ///
        /// **The reader the frame ring does not count.** An arrival stages its rows into the first
        /// copy and dispatches over them from a batch that rides whatever submit comes next — the
        /// placement's, in a game — and no trace stamps that read. The next placement into that
        /// copy waited for the frame that last traced it, which was submitted ahead of the arrival,
        /// and then wrote the rows from the host under a dispatch still reading them: the assert
        /// in `Buffer::writable` is what the game hit. `SkinTables::finishReads` is the wait, and
        /// it is measured off the tables' own stamp rather than the ring's.
        ///
        /// **A held submit, opened while this thread waits.** The wait cannot return before the
        /// hold opens, so it lasts at least the hold's length — and without it the placement would
        /// run at once, into rows a submit still reads, which the assert catches in a build that
        /// asserts and the bound catches in one that does not.
        TEST_F(RtxSkinPassTest, aPlacementWaitsForTheArrivalThatPosedTheFirstCopy)
        {
            Device& device = getDevice();
            CommandPool& pool = getPool();

            SceneDesc scene;

            const std::array oneRuns{ Shaders::runWord(0, 1), Shaders::runWord(0, 1), Shaders::runWord(0, 1),
                Shaders::runWord(0, 1) };
            const std::array oneInfluence{ Shaders::GpuInfluence{ .mBone = 0, .mWeight = 1.0f } };
            const RigSpec oneBone{ .mRuns = oneRuns, .mInfluences = oneInfluence, .mBones = 1 };

            const std::array upward{
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
            };
            const osg::BoundingBoxf anywhere(osg::Vec3f(), osg::Vec3f(1.0f, 1.0f, 1.0f));

            const DeformedMesh body = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = upward, .mIndices = Testing::sQuadIndices },
                {}, oneBone);
            const Index first = body.mMesh;
            const std::array atFive{ Testing::boneUp(5.0f) };
            Testing::poseRig(scene, first, atFive, anywhere);

            constexpr VkBufferUsageFlags readable
                = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            SlotBlocks poses{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };
            SlotBlocks normals{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };
            SlotBlocks tangents{ Shaders::VERTEX_BLOCK, sizeof(std::uint32_t) };
            poses.open(device, 2, readable, "posed positions");
            normals.open(device, 2, readable, "posed normals");
            tangents.open(device, 2, readable, "posed tangents");

            const SkinPass pass(device, Testing::getShaderDirectory());

            // The load: the tables and the room for one quad, into every copy.
            Batch load(pool);
            poses.reserve(load, 4);
            normals.reserve(load, 4);
            tangents.reserve(load, 4);
            SkinTables tables(device, load, scene, 2);
            load.flush();
            for (std::uint32_t slot = 0; slot < 2; ++slot)
            {
                poses.settle(FrameSlot{ slot });
                normals.settle(FrameSlot{ slot });
                tangents.settle(FrameSlot{ slot });
            }

            // The arrival: a second quad on the same rig, its rows staged into the first copy and
            // posed there, in a batch deferred to the next submit — which is held.
            scene.clearArrivals();
            scene.clearPlacement();
            const Index second = scene.addMesh(
                MeshArrays{ .mPositions = Testing::sUnitQuad, .mNormals = upward, .mIndices = Testing::sQuadIndices },
                {}, body.mDeformer);
            const std::array atTwo{ Testing::boneUp(2.0f) };
            Testing::poseRig(scene, second, atTwo, anywhere);
            ASSERT_EQ(scene.meshes().getArrived().size(), 1u);

            {
                Batch arrival(pool);
                poses.reserve(arrival, 8);
                normals.reserve(arrival, 8);
                tangents.reserve(arrival, 8);
                tables.extend(arrival, scene);
                EXPECT_TRUE(pass.recordArrived(arrival.getCommands(),
                    Skinning{ .mScene = scene,
                        .mSlot = FrameSlot{ 0 },
                        .mTables = tables,
                        .mPoses = poses,
                        .mNormals = normals,
                        .mTangents = tangents },
                    scene.meshes().getArrived()));
                arrival.defer();
            }

            Testing::HeldSubmit hold(device);
            const VkCommandBuffer carrier = pool.allocate(1).front();
            pool.begin(carrier);
            hold.submit(carrier);

            // Asked before the hold starts its clock, so the bound below is exact: the hold opens
            // no sooner than `held` after this, and the wait cannot return before it opens.
            constexpr std::chrono::milliseconds held{ 20 };
            const auto asked = std::chrono::steady_clock::now();
            hold.releaseAfter(held);

            tables.finishReads(FrameSlot{ 0 });
            EXPECT_GE(std::chrono::steady_clock::now() - asked, held)
                << "the placement did not wait for the arrival's submit";

            // The placement into the first copy, which writes the rows of every mesh it owes —
            // both quads, moved since the load — and reads the whole block back. A wait that
            // returned early records this over a submit the hold still keeps on the queue, which
            // is the write the assert fires on.
            const std::array atOne{ Testing::boneUp(1.0f) };
            Testing::poseRig(scene, first, atOne, anywhere);
            const std::array atThree{ Testing::boneUp(3.0f) };
            Testing::poseRig(scene, second, atThree, anywhere);

            const VkDeviceSize poseBytes = 8 * sizeof(osg::Vec3f);
            const Buffer read = Buffer::readBack(device, poseBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "test");
            pool.submitAndWait([&](VkCommandBuffer commands) {
                EXPECT_TRUE(pass.record(commands,
                    Skinning{ .mScene = scene,
                        .mSlot = FrameSlot{ 0 },
                        .mTables = tables,
                        .mPoses = poses,
                        .mNormals = normals,
                        .mTangents = tangents }));

                handOver(commands, Use::sBufferComputeWrite,
                    BufferUse{ VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT });
                poses.at(FrameSlot{ 0 }).getBlock(0).copyTo(commands, read, poseBytes);
            });

            const auto positionOf = [&](Index mesh, std::uint32_t vertex) {
                return readAt<osg::Vec3f>(read, scene.meshes().getRows()[mesh].mBindOffset + vertex);
            };
            for (std::uint32_t vertex = 0; vertex < 4; ++vertex)
            {
                EXPECT_EQ(positionOf(first, vertex), Testing::sUnitQuad[vertex] + osg::Vec3f(0.0f, 0.0f, 1.0f))
                    << vertex;
                EXPECT_EQ(positionOf(second, vertex), Testing::sUnitQuad[vertex] + osg::Vec3f(0.0f, 0.0f, 3.0f))
                    << vertex;
            }
        }
    }
}
