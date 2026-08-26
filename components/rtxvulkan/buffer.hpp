#pragma once

#include <cassert>
#include <cstring>
#include <span>

#include <vulkan/vulkan_core.h>

#include "memory.hpp"

namespace Rtx
{
    class Device;

    /// A `VkBuffer` and the allocation behind it.
    class Buffer
    {
    public:
        Buffer() = default;

        Buffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
        ~Buffer();

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;

        VkBuffer getHandle() const { return mHandle; }
        VkDeviceSize getSize() const { return mSize; }

        /// The GPU-side address, for the acceleration structure builder and for anything that
        /// dereferences a pointer in a shader. Only valid when the buffer was created with
        /// `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, which is asserted.
        VkDeviceAddress getDeviceAddress() const;

        void* map() const { return mMemory.map(); }
        void unmap() const { mMemory.unmap(); }

        /// Copies `data` to the start of a host-visible buffer.
        template <class T>
        void write(std::span<const T> data) const
        {
            const std::size_t bytes = data.size_bytes();
            assert(bytes <= mSize);

            void* mapped = map();
            std::memcpy(mapped, data.data(), bytes);
            unmap();
        }

    private:
        void destroy();

        VkDevice mDevice = VK_NULL_HANDLE;
        VkBuffer mHandle = VK_NULL_HANDLE;
        DeviceMemory mMemory;
        VkDeviceSize mSize = 0;
        bool mAddressable = false;
    };
}
