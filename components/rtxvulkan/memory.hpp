#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// `value` rounded up to the next multiple of `alignment`.
    ///
    /// **One statement of it, because every offset this renderer computes is a device offset.** An
    /// acceleration structure placed inside a shared buffer and a record placed inside a shader
    /// binding table are both laid out against a limit the driver states, and the arithmetic is the
    /// same one.
    inline VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

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

        DeviceMemory(const DeviceMemory&) = delete;
        DeviceMemory& operator=(const DeviceMemory&) = delete;

        /// **Written out, because the mapping has to be let go of with the allocation.** Everything
        /// else here empties itself; a pointer into memory the source no longer holds does not.
        DeviceMemory(DeviceMemory&& other) noexcept;
        DeviceMemory& operator=(DeviceMemory&& other) noexcept;

        VkDeviceMemory getHandle() const { return mHandle.get(); }

        /// The whole allocation, mapped, or null where the memory is not host-visible.
        ///
        /// **Mapped once at allocation and never unmapped.** The pointer a driver hands back does
        /// not move, so asking again is a call and a lock for an address already held — and memory
        /// need not be unmapped before it is freed.
        void* map() const { return mMapped; }

    private:
        Owned<VkDeviceMemory, vkFreeMemory> mHandle;
        void* mMapped = nullptr;
    };

    /// The index of a memory type satisfying `properties`, out of those `typeBits` allows.
    ///
    /// Throws when the device offers none: every combination this renderer asks for is guaranteed by
    /// the Vulkan specification on hardware that meets its requirements, so a failure here means the
    /// request was wrong, not the driver.
    std::uint32_t findMemoryType(const Device& device, std::uint32_t typeBits, VkMemoryPropertyFlags properties);
}
