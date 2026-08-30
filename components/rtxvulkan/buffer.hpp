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

        /// The whole buffer in main memory, or null where its memory is not host-visible.
        ///
        /// **Mapped once and left mapped, as `HostBuffer` is.** The pointer a driver hands back does
        /// not move, so asking for it again is a call and a lock for an address already held — and
        /// the frame that counts its hits asked for one twice a frame.
        void* map() const
        {
            assert(mMapped != nullptr && "a map of a buffer the host cannot reach");
            return mMapped;
        }

        /// Copies `data` to the start of a host-visible buffer.
        template <class T>
        void write(std::span<const T> data) const
        {
            const std::size_t bytes = data.size_bytes();
            assert(mMapped != nullptr && "a write to a buffer the host cannot reach");
            assert(bytes <= mSize);

            std::memcpy(mMapped, data.data(), bytes);
        }

    private:
        void destroy();

        VkDevice mDevice = VK_NULL_HANDLE;
        VkBuffer mHandle = VK_NULL_HANDLE;
        DeviceMemory mMemory;
        VkDeviceSize mSize = 0;

        /// Held from construction to destruction. Memory need not be unmapped before it is freed,
        /// which is what lets the mapping end with the allocation rather than with a call.
        void* mMapped = nullptr;

        bool mAddressable = false;
    };
}
