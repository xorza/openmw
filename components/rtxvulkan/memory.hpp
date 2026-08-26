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
        /// @param exportable whether another API may import this allocation. See `exportFd`.
        DeviceMemory(const Device& device, VkDeviceSize size, std::uint32_t typeBits, VkMemoryPropertyFlags properties,
            bool deviceAddress, bool exportable = false);

        /// An allocation another device or API already made, imported through `fd`.
        ///
        /// **Takes ownership of `fd` whether or not this succeeds**, which is what Vulkan does on
        /// success and what the failure path here does to match — a descriptor left behind by a
        /// throw is one nobody has a name for any more.
        ///
        /// @param typeBits the importing image's own `memoryTypeBits`. Intersected with what the
        ///        driver says the descriptor supports, because those are two different questions
        ///        and only the intersection is a legal answer.
        DeviceMemory(const Device& device, VkDeviceSize size, std::uint32_t typeBits, int fd);
        ~DeviceMemory();

        DeviceMemory(const DeviceMemory&) = delete;
        DeviceMemory& operator=(const DeviceMemory&) = delete;
        DeviceMemory(DeviceMemory&& other) noexcept;
        DeviceMemory& operator=(DeviceMemory&& other) noexcept;

        VkDeviceMemory getHandle() const { return mHandle; }

        /// The whole allocation, mapped. Only valid on host-visible memory, which is asserted.
        void* map() const;
        void unmap() const;

        /// A POSIX file descriptor another API can import this allocation through.
        ///
        /// **A new one every call, and the caller owns it.** `vkGetMemoryFdKHR` transfers ownership;
        /// an importer that does not take it — OpenGL's `glImportMemoryFdEXT` does — leaves a
        /// descriptor to close. Only valid where the memory was allocated `exportable`, which is
        /// asserted, and where the device has `VK_KHR_external_memory_fd`, which throws.
        int exportFd() const;

        /// How large the allocation actually is, which is what an importer has to be told.
        ///
        /// **Not width times height times bytes.** A driver pads and aligns an image's memory, and
        /// importing at the arithmetic size is a failed import or a torn texture.
        VkDeviceSize getSize() const { return mSize; }

    private:
        void destroy();

        VkDevice mDevice = VK_NULL_HANDLE;
        VkDeviceMemory mHandle = VK_NULL_HANDLE;
        VkDeviceSize mSize = 0;
        bool mHostVisible = false;
        bool mExportable = false;
    };

    /// The index of a memory type satisfying `properties`, out of those `typeBits` allows.
    ///
    /// Throws when the device offers none: every combination this renderer asks for is guaranteed by
    /// the Vulkan specification on hardware that meets its requirements, so a failure here means the
    /// request was wrong, not the driver.
    std::uint32_t findMemoryType(const Device& device, std::uint32_t typeBits, VkMemoryPropertyFlags properties);
}
