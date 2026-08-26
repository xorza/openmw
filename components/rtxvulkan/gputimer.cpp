#include "gputimer.hpp"

#include <array>
#include <cassert>
#include <vector>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// How many of the queue family's timestamp bits are meaningful, which is zero where the
        /// queue cannot timestamp at all.
        std::uint32_t validBits(VkPhysicalDevice device, std::uint32_t family)
        {
            std::uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

            return family < count ? families[family].timestampValidBits : 0;
        }
    }

    GpuTimer::GpuTimer(const Device& device)
        : mDevice(device)
    {
        const VkPhysicalDeviceLimits& limits
            = device.getPhysicalDevice().getProperties().mProperties2.properties.limits;

        const std::uint32_t bits = validBits(device.getPhysicalDevice().getHandle(), device.getQueueFamily());

        // A period of zero is the driver saying its clock does not advance, which no amount of
        // arithmetic recovers from.
        mSupported = bits > 0 && limits.timestampPeriod > 0.0f;
        if (!mSupported)
            return;

        mPeriod = limits.timestampPeriod;
        mMask = bits >= 64 ? ~std::uint64_t{ 0 } : (std::uint64_t{ 1 } << bits) - 1;

        const VkQueryPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = sMaxZones * 2,
            .pipelineStatistics = 0,
        };

        checkVk(vkCreateQueryPool(device.getHandle(), &info, nullptr, &mHandle), "vkCreateQueryPool");
        device.setName(VK_OBJECT_TYPE_QUERY_POOL, reinterpret_cast<std::uint64_t>(mHandle), "frame timestamps");

        mZones.reserve(sMaxZones);
        mSpans.reserve(sMaxZones);
    }

    GpuTimer::~GpuTimer()
    {
        if (mHandle != VK_NULL_HANDLE)
            vkDestroyQueryPool(mDevice.getHandle(), mHandle, nullptr);
    }

    void GpuTimer::beginFrame()
    {
        mZones.clear();
        mSpans.clear();
        mOpen = 0;
    }

    void GpuTimer::open(VkCommandBuffer commands, std::string_view name)
    {
        mDevice.beginLabel(commands, name);

        if (!mSupported)
            return;

        assert(mOpen == mZones.size() && "a zone was opened while another was still open");

        // A frame that wanted more zones than the pool holds is a frame being instrumented past what
        // this was built for; the label above still names it for a capture.
        if (mZones.size() >= sMaxZones)
            return;

        const auto first = static_cast<std::uint32_t>(mZones.size()) * 2;

        // **Reset here rather than once per command buffer.** The zones of one frame are spread over
        // three submits and this class is not told where the boundaries are; resetting the pair
        // about to be written, in the buffer about to write it, is correct wherever it lands.
        vkCmdResetQueryPool(commands, mHandle, first, 2);
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mHandle, first);

        mZones.push_back(Zone{ .mName = name, .mFirstQuery = first });
    }

    void GpuTimer::close(VkCommandBuffer commands)
    {
        mDevice.endLabel(commands);

        if (!mSupported || mOpen == mZones.size())
            return;

        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mHandle, mZones[mOpen].mFirstQuery + 1);
        ++mOpen;
    }

    std::span<const GpuSpan> GpuTimer::resolve()
    {
        assert(mOpen == mZones.size() && "a zone was left open when the frame was resolved");

        if (mZones.empty())
            return {};

        std::array<std::uint64_t, sMaxZones * 2> ticks{};
        const auto count = static_cast<std::uint32_t>(mZones.size()) * 2;

        // Waiting rather than polling for availability: every submit these were written into has
        // already been fenced, so the results are there and the flag costs nothing.
        checkVk(vkGetQueryPoolResults(mDevice.getHandle(), mHandle, 0, count, count * sizeof(std::uint64_t),
                    ticks.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
            "vkGetQueryPoolResults");

        mSpans.clear();
        for (const Zone& zone : mZones)
        {
            const std::uint64_t began = ticks[zone.mFirstQuery] & mMask;
            const std::uint64_t ended = ticks[zone.mFirstQuery + 1] & mMask;

            // The masked counter wraps, and a frame is nanoseconds against a counter that is at
            // least thirty-six bits: the difference is the elapsed time whichever side of a wrap the
            // two landed.
            const std::uint64_t elapsed = (ended - began) & mMask;

            mSpans.push_back(GpuSpan{ .mName = zone.mName, .mMs = static_cast<double>(elapsed) * mPeriod / 1.0e6 });
        }

        return mSpans;
    }
}
