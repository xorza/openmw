#pragma once

#include <cassert>
#include <cstring>
#include <span>

#include <vulkan/vulkan_core.h>

#include "memory.hpp"

namespace Rtx
{
    class Device;

    /// A buffer the device reads out of video memory and the host writes straight into.
    ///
    /// **What resizable BAR is for, and the reason nothing here stages.** The whole of this device's
    /// sixteen gigabytes is host-visible, so a table the frame rewrites needs no staging copy, no
    /// transfer command, no barrier and no submit — it is a `memcpy` into the memory the shader will
    /// read. Against the staging path it replaces, that is two allocations, a queue submit and a wait
    /// on the whole queue removed per table per frame.
    ///
    /// **Write-only, and the mapping is never handed out.** The memory is write-combining: sequential
    /// writes go at bus speed and a *read* of it is uncached, unprefetched and orders of magnitude
    /// slower than the host memory the caller built the data in. So this exposes no readable pointer
    /// and no way to accumulate in place — a caller assembles into an ordinary vector and copies once.
    ///
    /// **Nothing here synchronises, because the renderer has one frame in flight.** `renderFrame`
    /// ends in a submit and a wait on its fence, so the trace that read this buffer has finished
    /// before anything writes it again. A ring of staging blocks would be guarding a hazard this
    /// renderer does not have. A host write made before a submit is visible to that submit without a
    /// barrier, which is what makes the build commands that read these safe in the same recording.
    class HostBuffer
    {
    public:
        HostBuffer() = default;

        /// @param usage what the device does with it. `TRANSFER_DST` is neither needed nor added:
        ///        nothing copies into one of these.
        HostBuffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage);
        ~HostBuffer();

        HostBuffer(const HostBuffer&) = delete;
        HostBuffer& operator=(const HostBuffer&) = delete;
        HostBuffer(HostBuffer&& other) noexcept;
        HostBuffer& operator=(HostBuffer&& other) noexcept;

        VkBuffer getHandle() const { return mHandle; }
        VkDeviceSize getSize() const { return mSize; }

        /// Only valid where the buffer was created with `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`.
        VkDeviceAddress getDeviceAddress() const;

        /// Copies `data` to `offset` bytes in. The caller keeps to the buffer, which is asserted.
        template <class T>
        void writeAt(VkDeviceSize offset, std::span<const T> data) const
        {
            assert(mMapped != nullptr);
            assert(offset + data.size_bytes() <= mSize);

            std::memcpy(static_cast<std::byte*>(mMapped) + offset, data.data(), data.size_bytes());
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
            assert(mMapped != nullptr);
            std::memset(mMapped, 0, mSize);
        }

    private:
        void destroy();

        VkDevice mDevice = VK_NULL_HANDLE;
        VkBuffer mHandle = VK_NULL_HANDLE;
        DeviceMemory mMemory;
        VkDeviceSize mSize = 0;

        /// Mapped once and left mapped. Mapping is not free and this is rewritten every frame.
        void* mMapped = nullptr;

        bool mAddressable = false;
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
    void growTo(HostBuffer& held, const Device& device, VkDeviceSize bytes, VkBufferUsageFlags usage);
}
