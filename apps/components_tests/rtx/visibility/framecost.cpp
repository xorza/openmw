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

        /// A frame that changes nothing must not go to the heap.
        ///
        /// The concern is jitter rather than throughput: at sixty frames a second a single
        /// allocator stall is a dropped frame, and an average hides it. What this forbids on the
        /// frame path is a `std::string` built, an unreserved vector grown, a `std::function`
        /// captured, a `make_unique` reached for, or logging that did not compile out.
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

            const SceneDesc scene = makeWall();

            Device& device = *mHarness->mDevice;
            CommandPool pool(device);
            std::vector<InstanceRecord> records;
            makeInstanceRecords(scene, records);
            Batch setup(pool);
            Graveyard graveyard(device, pool);
            const SceneAcceleration acceleration(device, setup, scene, records, 1, graveyard);
            const SceneBuffers buffers(device, scene, records, 1, graveyard);

            const TextureArray textures(device, setup, 0, {}, graveyard);
            const SetLayout channelLayout = GBuffer::describeLayout(device);
            const SetLayout volumeLayout = FogVolume::describeLayout(device);
            VisibilityPass pass(
                device, setup, Testing::getShaderDirectory(), textures.getLayout(), channelLayout, volumeLayout, true);
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
                .mShading = textures.getShading(),
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

            // Everything a still frame does — the sea synthesised, the trace and the composite
            // recorded, the work submitted and waited on.
            // The camera is the same every time, which is what "steady" means — a moving one would
            // still allocate nothing, but then nothing would be pinned.
            const auto frame = [&] {
                const Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

                vkResetCommandBuffer(commands, 0);
                const VkCommandBufferBeginInfo begin{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                };
                vkBeginCommandBuffer(commands, &begin);
                waves.record(commands, camera.mTime);
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
                Rtx::awaitVk(device.getHandle(), finished, "a measured frame");
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
    }
}
