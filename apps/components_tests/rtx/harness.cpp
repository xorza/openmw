#include "harness.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/files/configurationmanager.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/memoryreport.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/result.hpp>
#include <components/rtxvulkan/barriers.hpp>
#include <components/rtxvulkan/graveyard.hpp>
#include <components/rtxvulkan/imageuse.hpp>
#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/requirements.hpp>
#include <components/rtxvulkan/result.hpp>
#include <components/rtxvulkan/validation.hpp>
#include <components/rtxvulkan/vulkanrenderer.hpp>

namespace Rtx::Testing
{
    namespace
    {
        /// Keyed on validation, which is the only axis any of these vary along.
        Once<Harness>& harnessCache(bool validation)
        {
            static Once<Harness> sValidated;
            static Once<Harness> sPlain;
            return validation ? sValidated : sPlain;
        }

        /// Keyed on validation, the way the devices are and for the same reason.
        Once<VulkanRenderer>& rendererCache(bool validation)
        {
            static Once<VulkanRenderer> sValidated;
            static Once<VulkanRenderer> sPlain;
            return validation ? sValidated : sPlain;
        }

        std::unique_ptr<Harness> build(bool validation, std::string& reason)
        {
            if (std::string obstacle = findInstanceObstacle(); !obstacle.empty())
            {
                reason = std::move(obstacle);
                return nullptr;
            }

            // Tests provoke errors deliberately and assert on them; aborting would take the suite
            // down with the first one. Synchronization validation is **the same switch
            // `describeRenderer` sets, for the same reason**: a test that drives Vulkan directly
            // supplies its own ordering with a submit and a wait, so a missing barrier in the code
            // under it shows as nothing at all — and a suite validated one way through the renderer
            // and another way beside it answers a different question in each file. It costs no
            // measurable time here either.
            const ValidationOptions options{
                .mLevel = validation ? ValidationLevel::Sync : ValidationLevel::Off,
                .mAbortOnError = false,
            };

            auto harness = std::make_unique<Harness>();
            harness->mInstance = std::make_unique<Instance>(options, std::span<const char* const>{});

            std::uint32_t count = 0;
            if (vkEnumeratePhysicalDevices(harness->mInstance->getHandle(), &count, nullptr) != VK_SUCCESS
                || count == 0)
            {
                reason = "no Vulkan device is installed";
                return nullptr;
            }

            harness->mDevice = std::make_unique<Device>(
                *harness->mInstance, PhysicalDevice::select(harness->mInstance->getHandle()), getPipelineCacheSpec());
            if (ValidationLog* log = harness->mInstance->getValidationLog(); log != nullptr)
                log->takeErrorsOnThisThread(harness->mMadeWith);

            return harness;
        }

        /// **The flag reaches the cache and the build together**, so the two cannot come apart: a
        /// device built unvalidated and filed under the validated key would be handed to every test
        /// in the suite.
        Harness* cachedHarness(bool validation, std::string& reason)
        {
            return harnessCache(validation).get(reason, [validation](std::string& why) {
                return build(validation, why);
            });
        }

        /// What the layers raised while the validated renderer was made: the other loads none.
        std::vector<std::string>& rendererMadeWith()
        {
            static std::vector<std::string> sMadeWith;
            return sMadeWith;
        }

        std::unique_ptr<VulkanRenderer> buildRenderer(bool validation, std::string& reason)
        {
            // Every test resizes to what it needs; one texel is only what the first target costs.
            try
            {
                auto renderer = std::make_unique<VulkanRenderer>(describeRenderer(1, 1, validation));
                if (validation)
                    renderer->takeValidationErrors(rendererMadeWith());
                return renderer;
            }
            catch (const Unsupported& obstacle)
            {
                reason = obstacle.what();
                return nullptr;
            }
        }

        VulkanRenderer* cachedRenderer(bool validation, std::string& reason)
        {
            return rendererCache(validation).get(reason, [validation](std::string& why) {
                return buildRenderer(validation, why);
            });
        }

        /// One half float, as the number it stands for.
        ///
        /// **Spelled out rather than shared with the renderer, and by arithmetic rather than by
        /// bits.** Several passes keep their output in halves, so a test that read them through the
        /// same helper the shader used would pass however wrong that helper was — and one written in
        /// shifts and masks is a second place for the subnormal case to be wrong.
        float fromHalf(std::uint16_t bits)
        {
            const float sign = (bits & 0x8000u) != 0 ? -1.0f : 1.0f;
            const int exponent = (bits >> 10) & 0x1f;
            const int mantissa = bits & 0x3ff;

            if (exponent == 0)
                return sign * std::ldexp(static_cast<float>(mantissa), -24);

            if (exponent == 31)
                return sign
                    * (mantissa == 0 ? std::numeric_limits<float>::infinity()
                                     : std::numeric_limits<float>::quiet_NaN());

            return sign * std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, exponent - 15);
        }

        /// What holding a device for the run costs the rest of the binary: death tests that exec
        /// rather than fork, and every device closed after the last test and before `main` returns.
        ///
        /// **A fork of a process holding a device runs to seconds**: its mappings are copied and
        /// the driver's fork handlers run, and gtest's default death test is a fork. Measured at
        /// four to eight seconds a death test once the pixel suite had built a renderer, against
        /// a fifth of a second for the re-exec the `threadsafe` style does — a child that never
        /// held a device.
        ///
        /// **Two Vulkan devices destroyed after `main` has returned abort inside the validation
        /// layer**, with no message and no stack of ours on it. One pair survives static destruction
        /// and a second does not — reproduced with nothing in the process but two instances left to
        /// exit — and this binary keeps up to four: a raw device for the tests that drive Vulkan
        /// directly and a `Renderer` for the pixel suite, each in a validated and an unvalidated
        /// flavour. Closing them here is both the fix and where they belonged: a cache that lives
        /// for the run should end with the run, not with the process.
        ///
        /// **And no device is a failed run, not an empty one.** Every fixture skips with a reason
        /// where the harness answers null, which is honest per test and a green run of nothing per
        /// suite: a machine without a driver passed forty files that opened no device. This binary
        /// holds only the tests that need one, so the first thing it does is ask for it.
        class DeviceEnvironment : public ::testing::Environment
        {
            void SetUp() override
            {
                GTEST_FLAG_SET(death_test_style, "threadsafe");

                std::string reason;
                if (getHarness(reason) == nullptr)
                    FAIL() << "rtx-gpu-tests needs a device and this machine has none: " << reason;
            }

            void TearDown() override
            {
                const std::string why = "the suite closed its devices after the last test";

                // The renderer before the raw devices, which is the order they were built in; neither
                // depends on the other.
                for (const bool validation : { true, false })
                    rendererCache(validation).release(why);
                for (const bool validation : { true, false })
                    harnessCache(validation).release(why);
            }
        };

        // Before `main`, because gtest only tears down environments registered before the run starts.
        [[maybe_unused]] const bool sRegistered = [] {
            ::testing::AddGlobalTestEnvironment(new DeviceEnvironment);
            return true;
        }();
    }

    std::string findInstanceObstacle()
    {
        std::uint32_t version = 0;
        if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < sApiVersion)
            return "the Vulkan loader is absent or older than this renderer requires";

        try
        {
            const Instance probe{ ValidationOptions{}, std::span<const char* const>{} };
        }
        catch (const Unsupported& obstacle)
        {
            return std::string("no Vulkan driver is installed: ") + obstacle.what();
        }

        return {};
    }

    Harness* getHarness(std::string& reason)
    {
        return cachedHarness(true, reason);
    }

    Harness* getUnvalidatedHarness(std::string& reason)
    {
        return cachedHarness(false, reason);
    }

    std::filesystem::path getShaderDirectory()
    {
        return std::filesystem::path(OPENMW_RTX_SHADER_DIR);
    }

    PipelineCacheSpec getPipelineCacheSpec()
    {
        // Silent, and built once: what is wanted is the path rule and not a configuration, and this
        // constructor reads no files to answer it.
        static const std::filesystem::path directory = Files::ConfigurationManager(true).getCachePath();

        return PipelineCacheSpec{ .mDirectory = directory, .mShaderDirectory = getShaderDirectory() };
    }

    RendererOptions describeRenderer(std::uint32_t width, std::uint32_t height, bool validation)
    {
        RendererOptions options;
        options.mShaderDirectory = getShaderDirectory();
        options.mCacheDirectory = getPipelineCacheSpec().mDirectory;
        options.mWidth = width;
        options.mHeight = height;
        // **Synchronization validation wherever the layers are, because a missing barrier is what
        // this suite is worst at seeing.** Every test here submits and waits, so the ordering a
        // frame relies on is supplied by the harness rather than by the code under test, and a
        // hazard shows as nothing at all — a traced view wrote its picture with no dependency on
        // the write before it for as long as there have been traced views. It costs no measurable
        // time in this suite.
        options.mValidation.mLevel = validation ? ValidationLevel::Sync : ValidationLevel::Off;
        // Tests provoke errors deliberately and assert on them; aborting would take the suite down
        // with the first one.
        options.mValidation.mAbortOnError = false;

        return options;
    }

    VulkanRenderer* getRenderer(std::string& reason)
    {
        return cachedRenderer(true, reason);
    }

    const std::vector<std::string>& getRendererMadeWith()
    {
        return rendererMadeWith();
    }

    VulkanRenderer* getUnvalidatedRenderer(std::string& reason)
    {
        return cachedRenderer(false, reason);
    }

    DeviceTest::DeviceTest(bool validation)
        : mValidation(validation)
    {
    }

    void DeviceTest::SetUp()
    {
        std::string reason;
        mHarness = mValidation ? getHarness(reason) : getUnvalidatedHarness(reason);
        if (mHarness == nullptr)
            GTEST_SKIP() << reason;

        // **Taken and then dropped**, because `takeErrorsOnThisThread` appends where a renderer's
        // own `takeValidationErrors` clears first: the log has to be emptied even though nothing
        // reads what comes off it here.
        takeRaised();
        mRaised.clear();
    }

    void DeviceTest::TearDown()
    {
        if (mHarness == nullptr)
            return;

        takeRaised();
        for (const std::string& error : mRaised)
            ADD_FAILURE() << "validation error: " << error;
    }

    void DeviceTest::takeRaised()
    {
        // Nothing at all where the layers are not loaded, which is the unvalidated device
        // `getUnvalidatedHarness` says why there is.
        if (ValidationLog* log = mHarness->mInstance->getValidationLog(); log != nullptr)
            log->takeErrorsOnThisThread(mRaised);
    }

    CommandPool& DeviceTest::getPool() const
    {
        return getDevice().getPool();
    }

    void RendererTest::SetUp()
    {
        std::string reason;
        mRenderer = getRenderer(reason);
        if (mRenderer == nullptr)
            GTEST_SKIP() << reason;

        forgetErrors(*mRenderer);
    }

    void RendererTest::TearDown()
    {
        if (mRenderer == nullptr)
            return;

        reportErrors(*mRenderer, "validation error");
    }

    void RendererTest::reportErrors(VulkanRenderer& renderer, std::string_view what)
    {
        renderer.takeValidationErrors(mErrors);
        for (const std::string& error : mErrors)
            ADD_FAILURE() << what << ": " << error;
    }

    void orderStorageWrites(VkCommandBuffer commands)
    {
        handOver(commands, Use::sBufferComputeWrite,
            BufferUse{ Use::sBufferComputeReadWrite.mStage | Use::sBufferHostRead.mStage,
                Use::sBufferComputeReadWrite.mAccess | Use::sBufferHostRead.mAccess });
    }

    HeldSubmit::HeldSubmit(const Device& device)
        : mDevice(device)
        , mGate(makeTimelineSemaphore(device, "test hold"))
    {
    }

    HeldSubmit::~HeldSubmit()
    {
        if (mOpener.joinable())
            mOpener.join();

        if (!mReleased)
            release();
    }

    std::uint64_t HeldSubmit::submit(VkCommandBuffer commands)
    {
        const VkSemaphoreSubmitInfo wait{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = mGate.get(),
            .value = 1,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        return mDevice.getPool().submit(commands, std::span(&wait, 1));
    }

    void HeldSubmit::releaseAfter(const std::chrono::milliseconds delay)
    {
        mOpener = std::thread([this, delay] {
            std::this_thread::sleep_for(delay);
            release();
        });
    }

    void HeldSubmit::release()
    {
        mReleased = true;

        const VkSemaphoreSignalInfo signal{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
            .semaphore = mGate.get(),
            .value = 1,
        };
        checkVk(mDevice, vkSignalSemaphore(mDevice.getHandle(), &signal), "vkSignalSemaphore");
    }

    Image makeTestImage(
        const Device& device, const VkExtent2D extent, const VkFormat format, const std::string_view name)
    {
        // `SAMPLED` because an upscaler samples its inputs and an image it cannot sample reads as
        // zero — no error, no validation message, a black frame. Both transfer bits so a clear can
        // fill it and the result can be read back.
        return Image(device, extent.width, extent.height, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            name);
    }

    std::vector<float> readHalves(const Image& image, std::uint32_t level)
    {
        std::vector<std::uint8_t> bytes;
        image.read(VK_IMAGE_LAYOUT_GENERAL, bytes, level);

        std::vector<float> values(bytes.size() / sizeof(std::uint16_t));
        for (std::size_t at = 0; at < values.size(); ++at)
        {
            std::uint16_t bits = 0;
            std::memcpy(&bits, bytes.data() + at * sizeof(bits), sizeof(bits));
            values[at] = fromHalf(bits);
        }

        return values;
    }

    BudgetLimit::BudgetLimit(MemoryAllocator& memory, const VkDeviceSize bytes)
        : mMemory(memory)
    {
        mMemory.limitBudget(bytes);
    }

    BudgetLimit::~BudgetLimit()
    {
        mMemory.limitBudget(std::nullopt);
    }

    VkDeviceSize budgetAbove(const MemoryAllocator& memory, const MemoryUse use, const VkDeviceSize above)
    {
        const std::uint32_t heap = memory.getVideoHeap();
        const HeapUse held = memory.report().mHeaps[heap];
        if (held.mHeld == 0)
            throw std::runtime_error("a device with no budget extension cannot say what the heap holds");

        VkDeviceSize owed = held.mHeld - held.mReserved;
        for (std::size_t before = 0; before < static_cast<std::size_t>(use); ++before)
            owed += memory.getHeld(heap, static_cast<MemoryUse>(before));

        return held.mHeld + owed + above;
    }

    NoRoomForContent::NoRoomForContent(const Device& device)
        : mNone(device.getMemory(), 0)
    {
        // Largest first, so the gaps fill in a few dozen buffers and not thousands, down to the
        // structure alignment, below which no resource content asks for is placed.
        for (VkDeviceSize size = VkDeviceSize{ 16 } << 20; size >= 256; size /= 16)
            while (true)
            {
                Result<Buffer, std::string_view> filler = Buffer::tryMake(MemoryUse::Texture, device,
                    BufferKind::DeviceLocal, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "content filler");
                if (!filler.isOk())
                    break;

                mFillers.push_back(std::move(filler.value()));
            }
    }
}
