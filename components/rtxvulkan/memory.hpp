#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    class Device;

    /// One `VkDeviceMemory` allocation.
    ///
    /// Deliberately one allocation per resource. That is the wrong shape for a renderer that creates
    /// thousands of small buffers, and this one does not: the scene arrives as a handful of large
    /// flat buffers, and every acceleration structure for a cell lives inside a single one of them at
    /// an offset. A suballocator would be answering a question nothing is asking.
    class DeviceMemory
    {
    public:
        DeviceMemory() = default;

        /// @param typeBits the `memoryTypeBits` from the resource's memory requirements.
        /// @param properties what the memory must be, e.g. device-local, or host-visible and coherent.
        /// @param deviceAddress whether the memory will back a buffer whose device address is taken.
        DeviceMemory(const Device& device, VkDeviceSize size, std::uint32_t typeBits, VkMemoryPropertyFlags properties,
            bool deviceAddress);
        ~DeviceMemory();

        DeviceMemory(const DeviceMemory&) = delete;
        DeviceMemory& operator=(const DeviceMemory&) = delete;
        DeviceMemory(DeviceMemory&& other) noexcept;
        DeviceMemory& operator=(DeviceMemory&& other) noexcept;

        VkDeviceMemory getHandle() const { return mHandle; }

        /// The whole allocation, mapped. Only valid on host-visible memory, which is asserted.
        void* map() const;
        void unmap() const;

    private:
        void destroy();

        VkDevice mDevice = VK_NULL_HANDLE;
        VkDeviceMemory mHandle = VK_NULL_HANDLE;
        bool mHostVisible = false;
    };

    /// The index of a memory type satisfying `properties`, out of those `typeBits` allows.
    ///
    /// Throws when the device offers none: every combination this renderer asks for is guaranteed by
    /// the Vulkan specification on hardware that meets its requirements, so a failure here means the
    /// request was wrong, not the driver.
    std::uint32_t findMemoryType(const Device& device, std::uint32_t typeBits, VkMemoryPropertyFlags properties);
}
