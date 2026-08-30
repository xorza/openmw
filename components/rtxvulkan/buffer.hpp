#pragma once

#include <cassert>
#include <cstring>
#include <span>

#include <vulkan/vulkan_core.h>

#include "memory.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// A `VkBuffer` and the allocation behind it.
    ///
    /// **Three kinds of memory and one type**, because what differs between them is where the
    /// allocation lives rather than what a buffer is. Which kind is asked for is the whole of the
    /// decision, so the three are named rather than spelled as a bitmask at every call site.
    class Buffer
    {
    public:
        /// A slot with nothing in it yet, which is what a table holds before it is first grown.
        Buffer() = default;

        /// Memory the device reads and the host cannot: everything a shader owns and every
        /// structure built for it.
        static Buffer deviceLocal(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage);

        /// Video memory the host writes straight into.
        ///
        /// **What resizable BAR is for, and the reason nothing here stages.** The whole of this
        /// device's sixteen gigabytes is host-visible, so a table the frame rewrites needs no
        /// staging copy, no transfer command, no barrier and no submit — it is a `memcpy` into the
        /// memory the shader will read. Against the staging path it replaces, that is two
        /// allocations, a queue submit and a wait on the whole queue removed per table per frame.
        ///
        /// **Write-only, which `map` enforces.** The memory is write-combining: sequential writes go
        /// at bus speed and a *read* of it is uncached, unprefetched and orders of magnitude slower
        /// than the host memory the caller built the data in. So a caller assembles into an ordinary
        /// vector and copies once.
        ///
        /// **Nothing here synchronises, because the owner keeps one of these per frame in flight.**
        /// `SceneBuffers` holds a copy of each table per frame slot and writes the copy the frame
        /// before last has finished with — `SlotTable` is what knows which rows each copy still owes
        /// — so the trace that read this buffer has finished before anything writes it again. A host
        /// write made before a submit is visible to that submit without a barrier, which is what
        /// makes the build commands that read these safe in the same recording.
        ///
        /// @param usage what the device does with it. `TRANSFER_DST` is neither needed nor added:
        ///        nothing copies into one of these.
        static Buffer hostWritten(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage);

        /// Host memory a copy is staged through, and the one kind the host may also read back.
        static Buffer staging(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage);

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&&) noexcept = default;
        Buffer& operator=(Buffer&&) noexcept = default;

        VkBuffer getHandle() const { return mHandle.get(); }
        VkDeviceSize getSize() const { return mSize; }

        /// The GPU-side address, for the acceleration structure builder and for anything that
        /// dereferences a pointer in a shader. Only valid when the buffer was created with
        /// `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, which is asserted.
        VkDeviceAddress getDeviceAddress() const;

        /// The whole buffer in main memory, for a caller that reads it back.
        ///
        /// **Only a staging buffer's, which is asserted.** Video memory the host writes is
        /// write-combined and reading it is orders of magnitude slower than reading the copy the
        /// caller wrote it from; `hostWritten` says the rest.
        void* map() const
        {
            assert(mReadable && "a read of memory that is written and never read back");

            return mMemory.map();
        }

        /// Copies `data` to `offset` bytes in. The caller keeps to the buffer, which is asserted.
        template <class T>
        void writeAt(VkDeviceSize offset, std::span<const T> data) const
        {
            void* const mapped = mMemory.map();
            assert(mapped != nullptr && "a write to a buffer the host cannot reach");
            assert(offset + data.size_bytes() <= mSize);

            std::memcpy(static_cast<std::byte*>(mapped) + offset, data.data(), data.size_bytes());
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
        void clear() const
        {
            void* const mapped = mMemory.map();
            assert(mapped != nullptr && "a clear of a buffer the host cannot reach");

            std::memset(mapped, 0, mSize);
        }

    private:
        Buffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
            bool readable);

        Owned<VkBuffer, vkDestroyBuffer> mHandle;

        /// Which holds the mapping: a table is rewritten every frame and mapping is not free, so the
        /// allocation takes its pointer once and hands it out.
        DeviceMemory mMemory;

        VkDeviceSize mSize = 0;
        bool mAddressable = false;

        /// Whether reading it back is what its memory is for. `map` is the only thing that asks.
        bool mReadable = false;
    };

    /// Grows `held` so it can hold `bytes`, and never leaves it holding nothing.
    ///
    /// **A descriptor a shader declares must have something bound to it**, and a table with nothing
    /// in it is still declared. Written the obvious way — grow if what is wanted does not fit — a
    /// table asked for nought bytes is never made at all, and the null handle reaches
    /// `vkCmdDispatch`: undefined, intermittent, and a lost device with no message. Three of these
    /// were doing it, one of them had a hand-written stand-in for it, and the rule that allowed it
    /// was the same in all seven places. It is this one now.
    ///
    /// Keeps whatever it already has where that is big enough, so a table settles at its high-water
    /// mark rather than being made again every frame.
    ///
    /// **Hands back what it displaced**, or an empty buffer where nothing was: a table too small
    /// may still be read by a frame in flight, so the caller buries it under that frame rather than
    /// letting it go here.
    [[nodiscard]] Buffer growTo(Buffer& held, const Device& device, VkDeviceSize bytes, VkBufferUsageFlags usage);
}
