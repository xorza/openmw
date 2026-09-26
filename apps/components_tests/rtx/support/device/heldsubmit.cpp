#include "heldsubmit.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <thread>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/result.hpp>

namespace Rtx::Testing
{
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
}
