#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/spanallocator.hpp>

#include "owned.hpp"

namespace Rtx
{
    class MemoryAllocator;

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

    /// Which side of `bufferImageGranularity` a resource sits on.
    ///
    /// A buffer is linear and an image of optimal tiling is not, and where the two share an
    /// allocation Vulkan requires a whole `bufferImageGranularity` between them.
    /// `MemoryAllocator` keeps a pool apiece, so they never share one and the rule is settled once
    /// rather than at every placement.
    enum class Tiling
    {
        Linear,
        Optimal,
    };

    /// A range of one device allocation, and the allocator that hands it back.
    ///
    /// **Not an allocation of its own.** A texture is two images and a cell brings a few hundred of
    /// them, so one `vkAllocateMemory` apiece is a kernel-visible call apiece — 1554 of them for the
    /// cell the game starts in, most for a shading map of two kilobytes that then pays the driver's
    /// whole granularity. `MemoryAllocator` hands these out instead, 24 allocations for that cell.
    class DeviceMemory
    {
    public:
        DeviceMemory() = default;
        ~DeviceMemory();

        DeviceMemory(const DeviceMemory&) = delete;
        DeviceMemory& operator=(const DeviceMemory&) = delete;

        /// **Written out, because a moved-from range must stop naming the block.** Everything else
        /// here empties itself; a range the source still described would be given back twice.
        DeviceMemory(DeviceMemory&& other) noexcept;
        DeviceMemory& operator=(DeviceMemory&& other) noexcept;

        VkDeviceMemory getHandle() const { return mHandle; }

        /// Where in that allocation the resource is bound, which is what `vkBind*Memory` takes.
        VkDeviceSize getOffset() const { return mOffset; }

        /// This range, mapped, or null where the memory is not host-visible.
        ///
        /// **The block is mapped once at allocation and never unmapped**, and this is that pointer
        /// with the range's own offset already added. The address a driver hands back does not move,
        /// so asking again is a call and a lock for an address already held — and memory need not be
        /// unmapped before it is freed.
        void* map() const { return mMapped; }

    private:
        friend class MemoryAllocator;

        DeviceMemory(MemoryAllocator& owner, std::uint32_t block, Span run, VkDeviceMemory handle, VkDeviceSize offset,
            void* mapped);

        MemoryAllocator* mOwner = nullptr;
        VkDeviceMemory mHandle = VK_NULL_HANDLE;
        VkDeviceSize mOffset = 0;
        void* mMapped = nullptr;
        Span mRun;
        std::uint32_t mBlock = 0;
    };

    /// Every `vkAllocateMemory` the renderer holds, and the ranges of them nothing is using.
    ///
    /// **The same shape as `StructureStorage`, over device memory rather than over one buffer.** A
    /// block is made once and never moved or freed, a `SpanAllocator` says where inside it a
    /// resource goes, and a range given back is merged with what it touches — so a cell that leaves
    /// hands its textures' memory to the cell that arrives without another call into the driver.
    ///
    /// **A pool per memory type and per tiling.** The type is what the resource's requirements and
    /// the caller's properties come to; the tiling is what keeps a buffer and an image off the same
    /// `bufferImageGranularity` page, which is the whole of that rule and costs at most one more
    /// block per type.
    ///
    /// **Not thread-safe, and nothing here asks it to be.** Every resource this renderer creates is
    /// created on the thread that records the frame.
    class MemoryAllocator
    {
    public:
        /// What a range's offset and length are counted in.
        ///
        /// Measured rather than assumed: every image this renderer creates asks for 1024-byte
        /// alignment, and every buffer for 16, 64 or 256. So a page of 1024 is the coarsest of them,
        /// and a resource never pays for an alignment it did not ask for. One that asks for more —
        /// which nothing on this hardware does, and which the code still has to answer — takes as
        /// many extra pages as its alignment exceeds one by.
        static constexpr VkDeviceSize sPage = 1024;

        /// @param memory the device's heaps and types, read once when the device was chosen.
        MemoryAllocator(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory);
        ~MemoryAllocator();

        MemoryAllocator(const MemoryAllocator&) = delete;
        MemoryAllocator& operator=(const MemoryAllocator&) = delete;

        /// Room for a resource with `requirements`, in memory that is `properties`.
        ///
        /// Throws `Unsupported` where the device offers no memory type that is both — every
        /// combination this renderer asks for is guaranteed by the Vulkan specification on hardware
        /// that meets its requirements, so a failure there means the request was wrong.
        DeviceMemory take(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties, Tiling tiling);

        /// How many calls to `vkAllocateMemory` stand behind everything handed out.
        std::size_t getBlockCount() const { return mBlocks.size(); }

    private:
        friend class DeviceMemory;

        /// One `vkAllocateMemory` and what has been handed out inside it.
        ///
        /// The allocator has no block boundary of its own: this allocation *is* the block, and what
        /// stops a range leaving it is `mPages` checked against `getEnd`.
        struct Block
        {
            Owned<VkDeviceMemory, vkFreeMemory> mHandle;
            void* mMapped = nullptr;
            SpanAllocator mRuns;
            std::uint32_t mPages = 0;
            std::uint32_t mPool = 0;
        };

        /// The index of a memory type satisfying `properties`, out of those `typeBits` allows.
        std::uint32_t findType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const;

        /// How large a block of `type` is made, before what a single resource may force.
        VkDeviceSize blockBytes(std::uint32_t type, std::uint32_t held) const;

        /// `run` in block `at`, as the range a resource of `alignment` is bound in.
        DeviceMemory place(std::uint32_t at, Span run, VkDeviceSize alignment);

        /// Gives a range back. Called by `DeviceMemory` and by nothing else.
        void give(std::uint32_t block, Span run);

        VkDevice mDevice = VK_NULL_HANDLE;
        const VkPhysicalDeviceMemoryProperties& mMemory;

        /// Every block of every pool, in one list. A block is never removed, so the index a range
        /// carries names the same block for the allocator's life — and a pool is the blocks whose
        /// `mPool` says so, which a walk of a few dozen entries finds without a table of its own.
        std::vector<Block> mBlocks;
    };
}
