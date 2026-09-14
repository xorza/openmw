#include "timeline.hpp"

#include <algorithm>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    Timeline::Timeline(const Device& device)
        : mDevice(device)
        , mHandle(makeTimelineSemaphore(device, "queue timeline"))
    {
    }

    bool Timeline::hasFinished(const std::uint64_t value) const
    {
        return value <= mFinished || value <= getFinished();
    }

    std::uint64_t Timeline::getFinished() const
    {
        std::uint64_t value = 0;
        checkVk(mDevice, vkGetSemaphoreCounterValue(mDevice.getHandle(), mHandle.get(), &value),
            "vkGetSemaphoreCounterValue");
        mFinished = value;
        return value;
    }

    void Timeline::waitFor(const std::uint64_t value, const char* const what) const
    {
        if (value <= mFinished)
            return;

        const VkSemaphore handle = mHandle.get();
        const VkSemaphoreWaitInfo wait{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &handle,
            .pValues = &value,
        };
        checkVkWait(mDevice, vkWaitSemaphores(mDevice.getHandle(), &wait, sPatience), what, sPatience);
        mFinished = std::max(mFinished, value);
    }

    VkSemaphoreSubmitInfo Timeline::signal(const std::uint64_t value) const
    {
        return VkSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = mHandle.get(),
            .value = value,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
    }
}
