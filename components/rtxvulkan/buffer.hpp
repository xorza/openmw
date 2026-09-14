#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "imageuse.hpp"
#include "memory.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// Which memory a buffer is made in — the three kinds this renderer uses, named rather than
    /// spelled as a property bitmask at every call site.
    enum class BufferKind
    {
        /// Memory the device reads and the host cannot: everything a shader owns and every
        /// structure built for it.
        DeviceLocal,

        /// Video memory the host writes straight into — what resizable BAR is for: the whole of
        /// this device's sixteen gigabytes is host-visible, so a table the frame rewrites is a
        /// `memcpy` and not two allocations, a submit and a wait. Write-only, which `map` enforces:
        /// the memory is write-combining and a read of it is orders of magnitude slower. Nothing
        /// here synchronises, because the owner keeps one per frame in flight and a host write made
        /// before a submit is visible to it without a barrier.
        HostWritten,

        /// Host memory a copy is staged through, and the one kind the host may also read back.
        Staging,
    };

    /// A `VkBuffer` and the allocation behind it.
    class Buffer
    {
    public:
        /// A slot with nothing in it yet, which is what a table holds before it is first grown.
        Buffer() = default;

        /// @param size may be nought: Vulkan has no zero-sized buffer, so a table with nothing in
        ///        it is made one byte long — which is what its descriptor needs and what nothing in
        ///        it has to read. A table asked for nought and never made was an address of nought
        ///        in the frame block, which is a lost device with no message.
        /// @param usage what the device does with it. `TRANSFER_DST` is not added for any kind.
        /// @param name what a capture and a validation message call it.
        static Buffer make(
            const Device& device, BufferKind kind, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name);

        /// `make`, for each kind by name.
        static Buffer deviceLocal(
            const Device& device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name);
        static Buffer hostWritten(
            const Device& device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name);
        static Buffer staging(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name);

        Buffer(Buffer&&) noexcept = default;
        Buffer& operator=(Buffer&&) noexcept = default;

        VkBuffer getHandle() const { return mHandle.get(); }
        VkDeviceSize getSize() const { return mSize; }

        bool isEmpty() const { return mHandle.get() == VK_NULL_HANDLE; }

        /// Which memory this is in, so what grows it makes the same kind.
        BufferKind getKind() const { return mKind; }

        /// Says a submit signalling `value` names this buffer. `addressFor` and `describe` say it
        /// for the next submit as they hand the buffer out; this is for a hand-out by handle — a
        /// copy's source, a vertex buffer bound — which names nothing on its own. What `isIdle`
        /// checks a host write against. `const`, because naming is not a change to the bytes, and
        /// the callers that name are const.
        void nameFor(std::uint64_t value) const { mNamedUntil = std::max(mNamedUntil, value); }

        /// The last value a submit naming this buffer signals, or nought where nothing has.
        std::uint64_t getNamedUntil() const { return mNamedUntil; }

        /// Whether every submit that names this buffer has run — what a host write of it asserts,
        /// because a host write over a submit still reading is the one hazard the layers cannot
        /// see. True of a buffer nothing has named, which is why every hand-out to a recording goes
        /// through `addressFor` or `describe`.
        bool isIdle() const;

        /// The GPU-side address, for a caller that compares or asserts on it and hands it to no
        /// recording — `addressFor` is for the rest. Only valid when the buffer was created with
        /// `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, which is asserted. Taken once at creation,
        /// because the frame block carries fourteen of them a frame.
        VkDeviceAddress getDeviceAddress() const
        {
            assert((mAddressable || mHandle.get() == VK_NULL_HANDLE) && "an address of a buffer not created for one");

            return mAddress;
        }

        /// The address as a recording takes it: names this buffer for the next submit, which is
        /// the one every recording this backend makes rides — a frame's own buffer, a placement, the
        /// interface, a deferred batch, a `submitAndWait`.
        VkDeviceAddress addressFor() const;

        /// The whole buffer as a descriptor, naming it for the next submit as `addressFor` does.
        VkDescriptorBufferInfo describe() const;

        /// The dependency between one use of the whole buffer and the next, for a caller collecting
        /// a run of them into one `Barriers`.
        VkBufferMemoryBarrier2 describeBarrier(const BufferUse& from, const BufferUse& to) const;

        /// The same, recorded on its own.
        void transition(VkCommandBuffer commands, const BufferUse& from, const BufferUse& to) const;

        /// Zeroes the first `bytes` on the queue, as a clear write, which is what `Use::sBufferClearWrite`
        /// orders against. Needs `TRANSFER_DST`.
        void clear(VkCommandBuffer commands, VkDeviceSize bytes = VK_WHOLE_SIZE) const;

        /// Copies the first `bytes` into the start of `into`, as a copy write.
        void copyTo(VkCommandBuffer commands, const Buffer& into, VkDeviceSize bytes) const;

        /// Records the dependency a host read of what the device wrote needs: a fence's access
        /// scope covers device access only, so a `map` after a wait can read what the caches
        /// happened to hold. Synchronization validation sees no `memcpy`, so a missing one shows
        /// up as a figure that is occasionally wrong. Recorded by whoever wrote the buffer.
        void orderForHostRead(VkCommandBuffer commands) const;

        /// Writes `bytes` from the start of the buffer inline in the command stream, in queue order,
        /// for the few hundred bytes a frame's constants are: ordered after `readers`' last read and
        /// write of it and before their next. One buffer then serves every frame.
        void updateInline(VkCommandBuffer commands, const BufferUse& readers, std::span<const std::byte> bytes) const;

        /// The whole buffer in main memory, for a caller that reads it back. Only a staging
        /// buffer's, which is asserted: `HostWritten` memory is write-combined.
        void* map() const
        {
            assert(mKind == BufferKind::Staging && "a read of memory that is written and never read back");

            return mMemory.map();
        }

        /// `count` elements of the buffer at `offset` bytes in, to be written in place and never
        /// read, for a caller that produces the bytes where they land — what MyGUI's `lock` and
        /// `unlock` are. `writeAt` is this for a caller that already holds them. Asserts that no
        /// submit still reads the buffer: a write into one a submit may still read goes through
        /// the queue, by `stageInto` or `updateInline`.
        template <class T>
        std::span<T> writable(VkDeviceSize offset, VkDeviceSize count) const
        {
            assert(isIdle() && "a host write over a buffer a submit still reads");

            return reach<T>(offset, count);
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

        /// Zeroes the whole buffer, for a block, which is made longer than what will be put in it:
        /// a buffer holding whatever was last in that memory is a picture that depends on it too.
        void clear() const { std::memset(writable<std::byte>(0, mSize).data(), 0, mSize); }

    private:
        Buffer(
            const Device& device, BufferKind kind, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name);

        /// `count` elements at `offset`, with nothing said about who is reading.
        template <class T>
        std::span<T> reach(VkDeviceSize offset, VkDeviceSize count) const
        {
            void* const mapped = mMemory.map();
            assert(mapped != nullptr && "a write to a buffer the host cannot reach");
            assert(offset + count * sizeof(T) <= mSize);

            return std::span<T>(reinterpret_cast<T*>(static_cast<std::byte*>(mapped) + offset), count);
        }

        /// Whose timeline the stamp is read against. Null for an empty buffer.
        const Device* mDevice = nullptr;

        Owned<VkBuffer, vkDestroyBuffer> mHandle;

        /// Which holds the mapping: a table is rewritten every frame and mapping is not free, so the
        /// allocation takes its pointer once and hands it out.
        DeviceMemory mMemory;

        VkDeviceSize mSize = 0;
        BufferKind mKind = BufferKind::DeviceLocal;
        bool mAddressable = false;
        VkDeviceAddress mAddress = 0;

        mutable std::uint64_t mNamedUntil = 0;
    };

    /// Grows `held` so it can hold `bytes`, and never leaves it holding nothing: an empty slot is
    /// grown whatever `bytes` is. Keeps whatever it already has where that is big enough. Hands
    /// back what it displaced, because a frame in flight may still be reading it.
    ///
    /// @param kind what to make, which is asserted to be what `held` already is where it holds
    ///        anything: a table does not change memory as it grows.
    [[nodiscard]] Buffer growTo(Buffer& held, const Device& device, BufferKind kind, VkDeviceSize bytes,
        VkBufferUsageFlags usage, std::string_view name);
}
