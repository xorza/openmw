#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>

#include <vulkan/vulkan_core.h>

#include "memory.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// A `VkBuffer` and the allocation behind it. Three kinds of memory and one type, named rather
    /// than spelled as a bitmask at every call site.
    class Buffer
    {
    public:
        /// A slot with nothing in it yet, which is what a table holds before it is first grown.
        Buffer() = default;

        /// Memory the device reads and the host cannot: everything a shader owns and every
        /// structure built for it.
        static Buffer deviceLocal(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage);

        /// Video memory the host writes straight into — what resizable BAR is for: the whole of
        /// this device's sixteen gigabytes is host-visible, so a table the frame rewrites is a
        /// `memcpy` and not two allocations, a submit and a wait. Write-only, which `map` enforces:
        /// the memory is write-combining and a read of it is orders of magnitude slower. Nothing
        /// here synchronises, because the owner keeps one per frame in flight and a host write made
        /// before a submit is visible to it without a barrier.
        ///
        /// @param usage what the device does with it. `TRANSFER_DST` is not added.
        static Buffer hostWritten(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage);

        /// Host memory a copy is staged through, and the one kind the host may also read back.
        static Buffer staging(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage);
        Buffer(Buffer&&) noexcept = default;
        Buffer& operator=(Buffer&&) noexcept = default;

        VkBuffer getHandle() const { return mHandle.get(); }
        VkDeviceSize getSize() const { return mSize; }

        /// The GPU-side address, for the acceleration structure builder and for anything that
        /// dereferences a pointer in a shader. Only valid when the buffer was created with
        /// `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, which is asserted. Taken once at creation,
        /// because the frame block carries fourteen of them a frame.
        VkDeviceAddress getDeviceAddress() const
        {
            assert((mAddressable || mHandle.get() == VK_NULL_HANDLE) && "an address of a buffer not created for one");

            return mAddress;
        }

        /// Records the dependency a host read of what the device wrote needs: a fence's access
        /// scope covers device access only, so a `map` after a wait can read what the caches
        /// happened to hold. Synchronization validation sees no `memcpy`, so a missing one shows
        /// up as a figure that is occasionally wrong. Recorded by whoever wrote the buffer.
        void orderForHostRead(VkCommandBuffer commands) const;

        /// The whole buffer in main memory, for a caller that reads it back. Only a staging
        /// buffer's, which is asserted: `hostWritten` memory is write-combined.
        void* map() const
        {
            assert(mReadable && "a read of memory that is written and never read back");

            return mMemory.map();
        }

        /// `count` elements of the buffer at `offset` bytes in, to be written in place and never
        /// read, for a caller that produces the bytes where they land — what MyGUI's `lock` and
        /// `unlock` are. `writeAt` is this for a caller that already holds them. Every host write
        /// goes through here, so a run that leaves the buffer is asked once.
        template <class T>
        std::span<T> writable(VkDeviceSize offset, VkDeviceSize count) const
        {
            void* const mapped = mMemory.map();
            assert(mapped != nullptr && "a write to a buffer the host cannot reach");
            assert(offset + count * sizeof(T) <= mSize);

            return std::span<T>(reinterpret_cast<T*>(static_cast<std::byte*>(mapped) + offset), count);
        }

        /// Copies `data` to `offset` bytes in.
        template <class T>
        void writeAt(VkDeviceSize offset, std::span<const T> data) const
        {
            std::memcpy(writable<T>(offset, data.size()).data(), data.data(), data.size_bytes());
        }

        template <class T>
        void write(std::span<const T> data) const
        {
            writeAt(0, data);
        }

        /// Zeroes the whole buffer.
        ///
        /// **For a block, which is made longer than what will be put in it.** A buffer holding
        /// whatever was last in that memory is a picture that depends on it too.
        void clear() const { std::memset(writable<std::byte>(0, mSize).data(), 0, mSize); }

    private:
        Buffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
            bool readable);

        Owned<VkBuffer, vkDestroyBuffer> mHandle;

        /// Which holds the mapping: a table is rewritten every frame and mapping is not free, so the
        /// allocation takes its pointer once and hands it out.
        DeviceMemory mMemory;

        VkDeviceSize mSize = 0;
        bool mAddressable = false;
        VkDeviceAddress mAddress = 0;

        /// Whether reading it back is what its memory is for. `map` is the only thing that asks.
        bool mReadable = false;
    };

    /// Grows `held` so it can hold `bytes`, and never leaves it holding nothing: a table asked for
    /// nought bytes and never made is an address of nought in the frame block, which is a lost
    /// device with no message. Keeps whatever it already has where that is big enough. Hands back
    /// what it displaced, because a frame in flight may still be reading it.
    [[nodiscard]] Buffer growTo(Buffer& held, const Device& device, VkDeviceSize bytes, VkBufferUsageFlags usage);
}
