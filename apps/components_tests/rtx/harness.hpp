#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/renderer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/pipelinecache.hpp>

namespace Rtx::Testing
{
    /// Instance and device for the whole test binary.
    ///
    /// Bring-up costs a good fraction of a second and nothing mutates it, so paying once is the
    /// difference between a suite that gets run and one that does not. Both are held by pointer so
    /// the instance can be built and inspected before there is a device to pair it with, and so it
    /// is destroyed last.
    struct Harness
    {
        std::unique_ptr<Instance> mInstance;
        std::unique_ptr<Device> mDevice;
    };

    /// Why this machine cannot build a Vulkan instance, or empty where it can.
    ///
    /// The two ways a machine legitimately has nothing to trace with: no loader, or a loader with no
    /// driver behind it — which fails at `vkCreateInstance` with `VK_ERROR_INCOMPATIBLE_DRIVER`
    /// rather than by handing back an empty device list. Both are a skip.
    ///
    /// Shared with the tests that build an instance of their own so the two cannot come to disagree
    /// about which failure is honest and which is a finding. Whether a device that *does* exist
    /// qualifies is a different question, and `PhysicalDevice::select` still throws it.
    std::string findInstanceObstacle();

    /// Something built once for the whole binary, and the reason where it could not be.
    ///
    /// **Held by whoever declares one rather than in a function-local static**, so that a gtest
    /// environment can close it while the process is still whole — see the teardowns in
    /// `harness.cpp` and `rtxtool/installation.cpp`, and the comment on `DeviceTeardown` for what
    /// static destruction did instead.
    template <class T>
    struct Once
    {
        std::unique_ptr<T> mValue;
        std::string mReason;
        bool mTried = false;

        template <class Build>
        T* get(std::string& reason, Build&& build)
        {
            if (!mTried)
            {
                mTried = true;
                mValue = build(mReason);
            }

            reason = mReason;
            return mValue.get();
        }

        /// Closes it, and says so to anything that asks afterwards rather than answering an empty
        /// reason — which a test would report as a skip with no explanation.
        void release(std::string why)
        {
            mValue.reset();
            mReason = std::move(why);
        }
    };

    /// Null when this machine has no Vulkan device at all, with `reason` saying so.
    ///
    /// A machine without a GPU legitimately cannot run these, and skipping is honest. A machine
    /// *with* one that does not meet the requirements is a finding, so that throws out of
    /// `PhysicalDevice::select` and fails the suite rather than skipping.
    Harness* getHarness(std::string& reason);

    /// The same, with no validation layers loaded.
    ///
    /// **The one thing in this suite that runs unvalidated, and it is measured rather than
    /// asserted.** `getAllocationCount` replaces the global `operator new`, so it cannot tell a
    /// layer's allocation from the renderer's; with the layers loaded, `RtxFrameCostTest` measures
    /// between 32,352 and 49,152 allocations over its 32 frames — a thousand to fifteen hundred a
    /// frame against a budget of nought. That is not a stricter test but a deleted one. Everything
    /// else is validated, so this second device is only built if something asks for it.
    Harness* getUnvalidatedHarness(std::string& reason);

    /// Where the build wrote the compiled shaders.
    std::filesystem::path getShaderDirectory();

    /// Where a device this suite makes keeps what it compiled, and what that cache is keyed on.
    ///
    /// **The user's own cache directory, the same one the game uses.** The whole point of keeping a
    /// pipeline cache is that a later process finds what an earlier one compiled, and a suite with a
    /// directory of its own is a suite that pays for a cold compile the game has already paid for.
    /// `PipelineCache` keys the file on the driver and on the shaders, so the two share it only
    /// while they agree about both.
    PipelineCacheSpec getPipelineCacheSpec();

    /// How every renderer in this suite is built, apart from the extent and the upscaler a caller
    /// sets for itself.
    ///
    /// **One place, so that a test standing up its own renderer cannot end up validated less than
    /// the shared one.** The one that legitimately is says so here rather than by clearing fields
    /// afterwards: the layers are two switches and a caller that remembered one of them would have a
    /// renderer half validated and no way to tell.
    ///
    /// @param validation off only for `getUnvalidatedRenderer`, which says why.
    RendererOptions describeRenderer(std::uint32_t width, std::uint32_t height, bool validation = true);

    /// The renderer the pixel tests trace through, built once for the binary.
    ///
    /// Null with `reason` where this machine cannot run the backend this build has — which is the
    /// ordinary case on a box developing the other one, and a skip rather than a failure.
    ///
    /// **What makes these tests an acceptance suite for any backend.** They assert hand-computed
    /// radiances, mip levels and transmittances, none of which is a statement about an API; a
    /// backend that passes this file is correct.
    ///
    /// **One for the binary, and a second is what a slow test is made of.** Measured with the
    /// on-disk pipeline cache warm: `createRenderer` costs 700–870 ms, the first `setScene` on the
    /// result costs another 900–1150 ms because it compiles every kernel the trace can need — see
    /// `VulkanRenderer::setScene` — and every `resize`, `setScene`, `renderFrame` and `readPixels`
    /// after that costs between one and fifteen. So a test that traces through this one is free and
    /// a test that stands up its own costs the suite two seconds. Only an upscaler needs its own,
    /// because the mode is fixed when the renderer is built.
    Renderer* getRenderer(std::string& reason);

    /// The same, with no validation layers loaded, for the one test that counts allocations.
    ///
    /// **A second renderer, for the reason `getUnvalidatedHarness` gives.** The layers allocate and
    /// `getAllocationCount` replaces the global `operator new`, so it cannot tell one of theirs from
    /// the renderer's — a frame measured through the shared renderer reports hundreds of allocations
    /// that no change to this code could remove. Built only if something asks, and asked by one test.
    Renderer* getUnvalidatedRenderer(std::string& reason);

    /// The base of a test that drives Vulkan directly.
    ///
    /// **One shape for the skip.** The suite had two: a fixture in some files and the same four
    /// lines written out in every test of the others. Which one a file used said nothing about the
    /// file, and a test that has to remember to ask for the reason is a test that can forget to.
    ///
    /// **The validation errors are drained before the test and reported after it**, the way
    /// `RendererTest` does and for the same reason. Three of the twenty-one fixtures over this base
    /// wrote that pair out for themselves and the other eighteen did not: a hazard the layers caught
    /// went onto a list nothing ever read. Draining first is how the slate is cleared — whatever a
    /// previous test left behind is not this one's to report.
    class DeviceTest : public ::testing::Test
    {
    protected:
        /// **Validated unless a derived fixture says otherwise**, which only a test that counts
        /// allocations wants: `getUnvalidatedHarness` says what the layers cost such a test.
        explicit DeviceTest(bool validation = true);

        void SetUp() override;

        void TearDown() override;

        Device& getDevice() const { return *mHarness->mDevice; }

        /// A pool on that device, opened on the first ask and closed with the test.
        CommandPool& getPool();

        Harness* mHarness = nullptr;

    private:
        void takeRaised();

        const bool mValidation;
        std::unique_ptr<CommandPool> mPool;
        std::vector<std::string> mRaised;
    };

    /// The base of a test that renders.
    ///
    /// **The validation errors are drained before the test and reported after it**, which two of the
    /// five files that render did not do at all: a hazard the layers caught went onto a list nothing
    /// ever read. Draining first is how the slate is cleared — whatever a previous test left behind
    /// is not this one's to report.
    class RendererTest : public ::testing::Test
    {
    protected:
        void SetUp() override;

        void TearDown() override;

        /// Drops whatever `renderer` is holding, so a test starts with nothing against it.
        ///
        /// **For a fixture with a renderer of its own**, which a renderer that upscales has to be:
        /// each one owns its instance and so its own log, and `mRenderer` is already done here.
        void forgetErrors(Renderer& renderer) { renderer.takeValidationErrors(mErrors); }

        /// Reports what `renderer` raised since `forgetErrors`, each failure headed by `what`.
        void reportErrors(Renderer& renderer, std::string_view what);

        Renderer* mRenderer = nullptr;

    private:
        std::vector<std::string> mErrors;
    };

    /// Orders one compute pass over a storage buffer against the next, and against a host read of
    /// what the last of them left.
    ///
    /// **Written as well as read, for the reason `WavePass::order` gives.** A transform that runs in
    /// place reads its buffer and writes it back, so what follows a pass is a write after a write as
    /// much as a read after one, and a dependency naming only the read leaves the two writes
    /// unordered. Three tests had written this barrier out for themselves and two of them named only
    /// the read — which is what a suite that never reported a validation error will hide.
    void orderStorageWrites(VkCommandBuffer commands);

    /// The four bytes at a pixel of an RGBA8 image `width` texels across, row zero at the top.
    ///
    /// **Beside `readHalves` because both are the read-back side**, and the two GUI test files each
    /// wrapped this arithmetic for themselves. Named for what it reads rather than for a texel:
    /// `terraincomposite.cpp` has a `texelOf` that indexes a `TextureData` by mip level and answers
    /// one packed integer.
    inline std::array<std::uint8_t, 4> rgbaAt(
        std::span<const std::uint8_t> pixels, std::uint32_t width, std::uint32_t x, std::uint32_t y)
    {
        const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;

        return { pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3] };
    }

    /// Every channel of one level of a half-float image, decoded, row major.
    ///
    /// **Left in the layout it was found in**, which `Image::read` promises: reading an image is not
    /// a change to it. Several passes keep their output in halves, so this is the read-back beside
    /// the decoder rather than one copy of it per suite.
    std::vector<float> readHalves(CommandPool& pool, const Image& image, std::uint32_t level = 0);
}
