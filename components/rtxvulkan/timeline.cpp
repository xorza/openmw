#include "timeline.hpp"

#include <algorithm>

#include <components/rtx/error.hpp>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    Timeline::Timeline(const Device& device)
        : mDevice(device)
    {
        const VkSemaphoreTypeCreateInfo type{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };
        const VkSemaphoreCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type,
        };
        checkVk(vkCreateSemaphore(device.getHandle(), &create, nullptr, mHandle.put(device.getHandle())),
            "vkCreateSemaphore");
        device.setName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<std::uint64_t>(mHandle.get()), "queue timeline");
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
        const VkResult result = vkWaitSemaphores(mDevice.getHandle(), &wait, sPatience);
        if (result == VK_TIMEOUT)
            throw Error(timedOut(what, sPatience));

        checkVk(mDevice, result, what);
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
