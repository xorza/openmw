#include "gputimer.hpp"

#include <array>
#include <cassert>
#include <vector>

#include "result.hpp"

namespace Rtx
{
    GpuTimer::GpuTimer(const Device& device)
        : mDevice(device)
    {
        const VkPhysicalDeviceLimits& limits
            = device.getPhysicalDevice().getProperties().mProperties2.properties.limits;

        // How many of the queue's timestamp bits are meaningful, which is zero where the queue
        // cannot timestamp at all — asked of the device once, when it was chosen.
        const std::uint32_t bits = device.getPhysicalDevice().getTimestampBits();

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
            .queryCount = sMaxGpuZones * 2,
            .pipelineStatistics = 0,
        };

        mHandle = QueryPool::make(device.getHandle(), vkCreateQueryPool, info, "vkCreateQueryPool");
        device.setName(mHandle.get(), "frame timestamps");

        mZones.reserve(sMaxGpuZones);
    }

    void GpuTimer::beginFrame(const std::uint64_t frame)
    {
        mZones.clear();
        mOpen = 0;
        mFrame = frame;
    }

    void GpuTimer::open(VkCommandBuffer commands, std::string_view name)
    {
        mDevice.beginLabel(commands, name);

        assert(mOpen == mZones.size() && "a zone was opened while another was still open");

        // A frame that wanted more zones than the pool holds is a frame being instrumented past what
        // this was built for; the label above still names it for a capture.
        if (mZones.size() >= sMaxGpuZones)
            return;

        const auto first = static_cast<std::uint32_t>(mZones.size()) * 2;
        const Zone& zone = mZones.emplace_back(
            Zone{ .mCheckpoint = Checkpoint{ .mName = name, .mFrame = mFrame }, .mFirstQuery = first });
        mDevice.checkpoint(commands, &zone.mCheckpoint);

        if (!mSupported)
            return;

        // Reset here rather than once per command buffer. The zones of one frame are spread over
        // three submits and this class is not told where the boundaries are; resetting the pair
        // about to be written, in the buffer about to write it, is correct wherever it lands.
        vkCmdResetQueryPool(commands, mHandle.get(), first, 2);
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mHandle.get(), first);
    }

    void GpuTimer::close(VkCommandBuffer commands)
    {
        mDevice.endLabel(commands);

        if (mOpen == mZones.size())
            return;

        if (mSupported)
            vkCmdWriteTimestamp2(
                commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mHandle.get(), mZones[mOpen].mFirstQuery + 1);
        ++mOpen;
    }

    void GpuTimer::resolve(GpuZones& into)
    {
        assert(mOpen == mZones.size() && "a zone was left open when the frame was resolved");

        into.clear();
        if (mZones.empty() || !mSupported)
            return;

        std::array<std::uint64_t, sMaxGpuZones * 2> ticks{};
        const auto count = static_cast<std::uint32_t>(mZones.size()) * 2;

        // Waiting rather than polling for availability: every submit these were written into has
        // already been fenced, so the results are there and the flag costs nothing.
        checkVk(mDevice,
            vkGetQueryPoolResults(mDevice.getHandle(), mHandle.get(), 0, count, count * sizeof(std::uint64_t),
                ticks.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
            "vkGetQueryPoolResults");

        for (const Zone& zone : mZones)
        {
            const std::uint64_t began = ticks[zone.mFirstQuery] & mMask;
            const std::uint64_t ended = ticks[zone.mFirstQuery + 1] & mMask;

            // The masked counter wraps, and a frame is nanoseconds against a counter that is at
            // least thirty-six bits: the difference is the elapsed time whichever side of a wrap the
            // two landed.
            const std::uint64_t elapsed = (ended - began) & mMask;

            into.add(GpuSpan{ .mName = zone.mCheckpoint.mName, .mMs = static_cast<double>(elapsed) * mPeriod / 1.0e6 });
        }
    }
}
