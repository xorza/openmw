#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <components/rtx/shadingmap.hpp>
#include <components/rtxvulkan/scenemicromaps.hpp>
#include <components/rtxvulkan/spritebinpass.hpp>

#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// Its own device, because the validation layers allocate and this test counts allocations.
        class RtxFrameCostTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                std::string reason;
                mHarness = Testing::getUnvalidatedHarness(reason);
                if (mHarness == nullptr)
                    GTEST_SKIP() << reason;
            }

            Testing::Harness* mHarness = nullptr;
        };

        /// A frame that changes nothing but a body's pose must not go to the heap.
        ///
        /// The concern is jitter rather than throughput: at sixty frames a second a single
        /// allocator stall is a dropped frame, and an average hides it. What this forbids on the
        /// frame path is a `std::string` built, an unreserved vector grown, a `std::function`
        /// captured, a `make_unique` reached for, or logging that did not compile out.
        ///
        /// **A body moving every frame, because that is the one thing the frame path computes
        /// rather than copies.** A skinned mesh is posed by rows the host writes and a dispatch the
        /// device runs, then refitted, then traced — and each of those is a place a vector could
        /// grow. A wall alone would pin the trace and leave the pose unmeasured.
        ///
        /// Warmed up first, because the first of anything legitimately allocates: descriptor pools
        /// grow, the driver caches its first call, and a command buffer finds its size.
        TEST_F(RtxFrameCostTest, aSteadyFrameDoesNotTouchTheHeap)
        {
            constexpr std::uint32_t size = 64;
            constexpr int warmUpFrames = 8;
            constexpr int measuredFrames = 32;

            // What a frame is allowed, and why. Zero is the claim; a constant rather than a literal
            // zero is so that a path which must allocate can be admitted deliberately, with the
            // number and the reason written down beside it.
            constexpr std::size_t budgetPerFrame = 0;

            SceneDesc scene = makeWall();

            // A second wall behind the first, skinned to one bone, so that every frame has a body
            // to pose: the wall's own bind pose stands a hundred units behind the camera's wall and
            // the bone walks it one unit further every frame.
            const Index body = scene.addMesh(sWallQuad, {}, {}, sQuadIndices, {}, Deform::Rig, addOneBoneRig(scene, 4));
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::translate(0.0f, 100.0f, 0.0f), .mMesh = body });
            poseByOneBone(scene, body, osg::Matrixf::identity());

            Device& device = *mHarness->mDevice;
            CommandPool pool(device);
            std::vector<InstanceRecord> records;
            std::vector<Index> changed;
            makeInstanceRecords(scene, records);
            Batch setup(pool);
            Graveyard graveyard(device, pool);
            SceneAcceleration acceleration(device, scene, 1);
            SceneBuffers buffers(device, scene, records, 1, graveyard);
            SkinTables skinTables(device, scene, 1, graveyard);
            const SkinPass skin(device, Testing::getShaderDirectory());

            // Nothing here is a sprite either, and the bin still runs every frame: the list a
            // frame with no sprites reads is one the pass writes.
            const SpriteBinPass spriteBin(device, Testing::getShaderDirectory());

            // Nothing here is a cutout, so nothing is baked; what a placement asks of this every
            // frame is the refit's description, which must not allocate either.
            const SceneMicromaps micromaps(device);

            // Posed and then built, as the renderer builds a scene.
            skin.record(
                setup.getCommands(), scene, 0, skinTables, acceleration.getPositions(), buffers.getNormals(), nullptr);
            acceleration.build(setup, scene, records, micromaps, graveyard);

            const TextureArray textures(device, setup, 0, {}, graveyard);
            const SetLayout channelLayout = GBuffer::describeLayout(device);
            const SetLayout volumeLayout = FogVolume::describeLayout(device);
            // Reordered, because that is the most the frame path can be asked to do: a mode that
            // records a hit object and hints a sort reaches more of the shader than one that does
            // neither, and what is claimed here is about every frame this renderer can record.
            VisibilityPass pass(device, setup, Testing::getShaderDirectory(), textures.getLayout(), channelLayout,
                volumeLayout, true, Reorder::Both);
            setup.flush();

            // **Built here and not inside the frame**, which is where the sea's own allocation is:
            // the spectrum is drawn once for a state and a frame turns its phases, so a steady
            // frame reaches this pass without reaching the heap.
            const WavePass waves(device, pool, Testing::getShaderDirectory());

            // The fog's field is drawn once for the life of the device, so a steady frame reaches it
            // the same way.
            const FogTile fog(device, pool);

            // And the volume it is integrated into belongs to the camera's size, so a steady frame
            // finds the same one every time.
            const FogVolume fogVolume(device, pool, volumeLayout, size, size);

            const VisibilityInputs inputs{
                .mScene = acceleration.getTopLevel(),
                .mBuffers = &buffers,
                .mIndexBlocks = acceleration.getIndexBlocks(),
                .mTextures = textures.getSet(),
                .mWaves = &waves,
                .mFog = &fog,
                .mFogVolume = &fogVolume,
            };

            const GBuffer channels(device, channelLayout, size, size);
            const CompositePass composite(device, pool, Testing::getShaderDirectory());

            // The format the shader declares for `colour`. A narrower one makes every load and store
            // through it undefined for the whole image, which the layers say and nothing else does.
            Image target(device, size, size, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "cost-target");
            const Buffer hits = Buffer::staging(device, sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

            // Once, so that the frames below start from laid-out images rather than each paying
            // transitions the real loop pays at startup.
            pool.submitAndWait([&](VkCommandBuffer commands) {
                channels.begin(commands);
                target.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            });

            // A command buffer reused against a fence, which is what a frame is and what the window
            // loop does. `submitAndWait` would be the setup shape — a fresh buffer from the pool and
            // a wait on the whole queue — and it is measured here as allocating nothing either, so
            // this is about what is being pinned rather than about what it costs today.
            const std::vector<VkCommandBuffer> recorded = pool.allocate(1);
            const VkCommandBuffer commands = recorded.front();

            const VkFenceCreateInfo describeFence{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkFence finished = VK_NULL_HANDLE;
            ASSERT_EQ(vkCreateFence(device.getHandle(), &describeFence, nullptr, &finished), VK_SUCCESS);

            // Everything a frame with a body in it does — the body posed, the sea synthesised, the
            // pose dispatched, the structures refitted and rebuilt, the trace and the composite
            // recorded, the work submitted and waited on.
            // The camera is the same every time, which is what "steady" means — a moving one would
            // still allocate nothing, but then nothing would be pinned.
            float walked = 0.0f;
            const auto frame = [&] {
                const Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

                // What the game's walk does to a body every frame: a new pose, compared and taken.
                walked += 1.0f;
                scene.clearPlacement();
                poseByOneBone(scene, body, osg::Matrixf::translate(0.0f, walked, 0.0f));
                updateInstanceRecords(scene, records, changed);

                vkResetCommandBuffer(commands, 0);
                const VkCommandBufferBeginInfo begin{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                };
                vkBeginCommandBuffer(commands, &begin);

                // The placement, as `VulkanRenderer::recordPlacement` records it: the pose, the
                // refit and the top level, then the tables.
                skin.record(commands, scene, 0, skinTables, acceleration.getPositions(), buffers.getNormals(), nullptr);
                acceleration.place(
                    scene, records, changed, micromaps, Rtx::Placing{ .mCommands = commands, .mGraveyard = graveyard });
                buffers.place(scene, records, changed, 0, graveyard);
                scene.advancePlacement();

                waves.record(commands, camera.mTime);
                buffers.binSprites(spriteBin, camera.mOrigin, camera.mCamera, camera.mSunPosition,
                    Rtx::Placing{ .mCommands = commands, .mGraveyard = graveyard });
                channels.begin(commands);
                pass.record(commands, inputs, channels, hits, camera, true, nullptr);
                channels.handOver(commands);
                // No history: a still frame averages nothing, and the pass stands in for the
                // binding rather than making the caller carry an image it never reads.
                composite.record(commands, channels, channels.getIndirect(), nullptr, target,
                    Shaders::CompositeConstants{ .mWidth = size, .mHeight = size, .mAccumulate = 0 });
                vkEndCommandBuffer(commands);

                const VkCommandBufferSubmitInfo buffer{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                    .commandBuffer = commands,
                };
                const VkSubmitInfo2 submit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                    .commandBufferInfoCount = 1,
                    .pCommandBufferInfos = &buffer,
                };
                vkQueueSubmit2(device.getQueue(), 1, &submit, finished);

                // **Its own submit, but not its own wait.** The shape above is the point of the test
                // — a command buffer reused against a fence is what a frame is — so it does not go
                // through `submitAndWait`, which allocates a buffer per call. The wait is another
                // matter: forty of them with no deadline is what turned one stalled submit into a
                // suite that never finished and a process that had to be killed.
                Rtx::awaitVk(device, finished, "a measured frame");
                vkResetFences(device.getHandle(), 1, &finished);
            };

            for (int i = 0; i < warmUpFrames; ++i)
                frame();

            const std::size_t before = Testing::getAllocationCount();
            for (int i = 0; i < measuredFrames; ++i)
                frame();
            const std::size_t after = Testing::getAllocationCount();

            vkDestroyFence(device.getHandle(), finished, nullptr);

            EXPECT_LE(after - before, budgetPerFrame * measuredFrames)
                << (after - before) << " allocations across " << measuredFrames << " frames";
        }

        /// A texture arriving costs the device objects it is and nothing besides.
        ///
        /// **The arrival frame is the frame with the least room, which is why this is bounded at
        /// all.** A cell crossing writes a couple of hundred textures into the array on one frame,
        /// and a vector the write grows for itself is growth on top of the upload. What this leaves
        /// is what a texture *is*: two images and the staging buffer their bytes travel in, each of
        /// which is a device allocation the host has to hold a handle for.
        ///
        /// Warmed up with one texture first, so the array's own scratch and its descriptor pool have
        /// reached their size and what is measured is the arrival and not the array.
        TEST_F(RtxFrameCostTest, aTextureArrivingCostsWhatATextureIs)
        {
            constexpr std::uint32_t extent = 4;
            constexpr std::uint32_t slots = 64;
            constexpr std::uint32_t measured = 32;

            // What one texture is allowed, and what it is spent on. Two images, each a `unique_ptr`
            // the host holds for a device object; the staging buffer its bytes travel in; and the
            // names those carry, which a build that names objects makes strings for and a release
            // build does not — `Device::wantsNames`. Measured at five apiece with names on.
            //
            // The sixth is the batch's own list of what it is keeping alive and the graveyard's, both
            // of which grow as any vector does. Nothing here may grow per texture: that is what the
            // number is for, and a seventh is a buffer somebody made per arrival.
            constexpr std::size_t budgetPerTexture = 6;

            Device& device = *mHarness->mDevice;
            CommandPool pool(device);
            Graveyard graveyard(device, pool);

            // Flat and uncompressed, so the description is exact arithmetic rather than a file.
            const std::vector<std::uint8_t> texels(std::size_t{ extent } * extent * 4, 0xFF);
            const std::array<MipLevel, 1> levels{ MipLevel{ 0, extent, extent } };
            const std::array<float, ShadingMap::sCells> shading{};

            const auto describe = [&](std::uint32_t slot) {
                return TextureData{
                    .mSlot = slot,
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = extent,
                    .mHeight = extent,
                    .mBytes = std::as_bytes(std::span(texels)),
                    .mLevels = levels,
                    .mShading = shading,
                    .mName = "arrival",
                };
            };

            Batch setup(pool);
            TextureArray array(device, setup, slots, {}, graveyard);
            setup.flush();

            const auto arrive = [&](Batch& batch, std::uint32_t slot) {
                const TextureData described = describe(slot);
                array.write(batch, std::span(&described, 1), graveyard);
            };

            {
                Batch warm(pool);
                arrive(warm, 0);
                warm.flush();
            }

            // **One batch for all of them, which is how a cell arrives**: the renderer records every
            // texture of a crossing into one and flushes once. A batch apiece would be measuring the
            // command pool rather than the array.
            Batch batch(pool);

            const std::size_t before = Testing::getAllocationCount();
            for (std::uint32_t slot = 1; slot <= measured; ++slot)
                arrive(batch, slot);
            const std::size_t after = Testing::getAllocationCount();

            batch.flush();

            EXPECT_LE(after - before, budgetPerTexture * measured)
                << (after - before) << " allocations across " << measured << " arrivals, "
                << static_cast<double>(after - before) / measured << " apiece";

            EXPECT_EQ(array.getHeld().mCount, measured + 1);
        }
    }
}
