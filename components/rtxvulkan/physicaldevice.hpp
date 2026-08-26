#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "requirements.hpp"

namespace Rtx
{
    /// A physical device that met every requirement, with what it reported about itself.
    ///
    /// Selection is deliberately unforgiving: a device that lacks a required extension or feature is
    /// rejected with that name in the message rather than silently demoted to a lesser path. There
    /// is no lesser path.
    class PhysicalDevice
    {
    public:
        /// Picks a device, preferring discrete over anything else.
        ///
        /// Throws `Error` listing every candidate and what each was missing when none qualifies —
        /// the one moment where a wall of text is the useful answer.
        static PhysicalDevice select(VkInstance instance);

        VkPhysicalDevice getHandle() const { return mHandle; }

        const DeviceProperties& getProperties() const { return *mProperties; }

        /// Queue family with graphics and compute, which on the target hardware is also the one
        /// that can present. A separate transfer queue is an M12 question.
        std::uint32_t getQueueFamily() const { return mQueueFamily; }

        /// Which of `getOptionalDeviceExtensions()` this device offers, in that order.
        const std::vector<const char*>& getAvailableOptionalExtensions() const { return mOptionalExtensions; }

        /// Multi-line report for `openmw-rtxtool info`.
        std::string describe() const;

    private:
        PhysicalDevice() = default;

        VkPhysicalDevice mHandle = VK_NULL_HANDLE;

        // By pointer so a move leaves the internal pNext chain pointing at the same memory.
        std::unique_ptr<DeviceProperties> mProperties;

        std::uint32_t mQueueFamily = 0;
        std::vector<const char*> mOptionalExtensions;
        VkDeviceSize mDeviceLocalMemory = 0;
    };
}
